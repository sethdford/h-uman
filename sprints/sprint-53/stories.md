---
title: "Sprint 53 — Sprint C / C5: Getting-started flow + docs"
created: 2026-05-24
sprint: 53
branch: sprint-53-getting-started-docs
spec: docs/plans/2026-05-25-sprint-c-backlog.md (§C5, lines 255-291)
program: docs/plans/2026-05-25-sprint-c-backlog.md
status: planned (PO + design only — implementation deferred to a follow-up multi-session work unit)
---

# Sprint 53 — Getting-started flow + docs

## Goal

Even a perfect installer (C1) and wizard (C2) won't reach users who
can't find them. Sprint 53 ties the surfaces together with a
single canonical `getting-started.md` page that's the FIRST link in
the README, supplemented by a `human help getting-started`
interactive in-CLI guide for users already in a terminal.

This sprint is intentionally smaller than C1/C2/C3/C4 — it's mostly
markdown + a small CLI handler. The work that matters is
**curation**, not code. The risk is shipping docs that read like
docs (long, complete, comprehensive) instead of the get-out-of-the-
way one-page README → 5-minute-success path.

## Scope discipline

- **In scope:** `docs/guides/getting-started.md`, a 5-screenshot
  walkthrough, `human help getting-started` interactive CLI handler,
  README rewrite to put install + getting-started at the top, plus
  a `human help troubleshoot` quick-help command.
- **Out of scope:** video walkthroughs, an external docs site, blog
  posts, interactive web tutorials, persona authoring guides
  (that's a Sprint D topic), API documentation generators.

## Wave plan (deferred to implementation sprint)

```
US-C5.1 (canonical getting-started.md + screenshots)  ──┐
                                                        ├── US-C5.2 (README rewrite — install + getting-started first)
                                                        ├── US-C5.3 (`human help getting-started` CLI handler)
                                                        └── US-C5.4 (`human help troubleshoot` quick-help)
                                                                  │
                                                                  ↓
                                                        US-C5.5 (link-rot + freshness CI gate)
```

US-C5.1 is the source-of-truth doc; everything else points at it.
US-C5.2/C5.3/C5.4 can be parallel once C5.1 lands. US-C5.5 is the
gate that prevents the docs from rotting.

Estimate matches backlog: **~800 LoC (mostly markdown), 1 multi-session work unit.**

## Stories

### Story US-C5.1 — Canonical `getting-started.md` (single page)

**As** a curious user landing on the GitHub repo,
**I want** ONE page that takes me from "I see the README" to
            "I successfully sent a test message" in five minutes or
            less, with screenshots of every step,
**So that** I don't have to assemble the journey from five separate
            docs + a forum post.

**Acceptance criteria:**

1. `docs/guides/getting-started.md` (NEW). Length cap: 400 lines.
   If we can't fit getting-started in 400 lines we've failed at
   curation.
2. Section structure (strict):
   - "What is human?" (3 lines max — the elevator pitch)
   - "Install" (one paragraph + link to `installation.md` from
     Sprint 49)
   - "First launch" (what you'll see — links to
     `onboarding-step{1..5}.md` copy files from Sprint 51)
   - "Verify it works" (single command: `human doctor`, link to
     `doctor.md` from Sprint 50)
   - "What to do if something fails" (link to `troubleshoot.md` +
     `human help troubleshoot`)
   - "Next steps" (3-4 things — adding more channels, customizing
     persona, reading the privacy doc)
3. Five screenshots in `docs/guides/img/getting-started/`:
   - `01-onboard-welcome.png` (Step 1 of the wizard)
   - `02-onboard-provider.png` (Step 2 with green ✓)
   - `03-onboard-channel.png` (Step 4 selecting iMessage)
   - `04-test-send.png` (Step 5 successful test)
   - `05-doctor-pass.png` (`human doctor` all-green output)
4. Every code block in the page is copy-pasteable and tested by
   US-C5.5 (link-rot CI gate).
5. Page is plain markdown (no Jekyll-specific syntax, no React
   components) — renders correctly on GitHub web view + local
   editor.

**Files expected to change:**

- `docs/guides/getting-started.md` (NEW, ~400 lines markdown)
- `docs/guides/img/getting-started/01-onboard-welcome.png` (NEW)
- `docs/guides/img/getting-started/02-onboard-provider.png` (NEW)
- `docs/guides/img/getting-started/03-onboard-channel.png` (NEW)
- `docs/guides/img/getting-started/04-test-send.png` (NEW)
- `docs/guides/img/getting-started/05-doctor-pass.png` (NEW)

---

### Story US-C5.2 — README.md rewrite (install + getting-started first)

**As** a visitor scanning the GitHub repo for 10 seconds,
**I want** the README's FIRST visible content (above the fold on a
            laptop browser) to be the install command + a single
            prominent link to getting-started.md,
**So that** I don't have to scroll past 200 lines of project
            philosophy to find out how to actually try the thing.

**Acceptance criteria:**

1. README.md restructured to put `## Quick start` as the FIRST
   section after the project tagline + logo:
   ```
   ## Quick start
   1. **Install**: `brew install human` (or download the .pkg from
      Releases)
   2. **Setup**: `human` (auto-runs onboarding on first launch)
   3. **Verify**: `human doctor`
   4. **Read more**: [Getting started →](docs/guides/getting-started.md)
   ```
2. Existing sections (philosophy, architecture, design decisions)
   move BELOW Quick start. They're not removed — just reordered.
3. The "Why this exists" / strategic-mission sections move to a
   separate `docs/why.md` linked from Quick start as "Why human?
   →". README isn't deleted of philosophy; it just stops being the
   landing page for it.
4. Test: a script (`scripts/check-readme-structure.sh`, NEW) parses
   the README and asserts:
   - `## Quick start` is the first h2 section
   - Quick start contains the four expected items
   - The getting-started.md link is present and resolves
5. Pre-commit hook: if `README.md` is staged AND the script fails,
   block the commit.

**Files expected to change:**

- `README.md` (rewrite — net change ~150 lines moved, ~30 added)
- `docs/why.md` (NEW, ~200 lines — the philosophy content displaced
  from README)
- `scripts/check-readme-structure.sh` (NEW, ~50 lines)
- `.githooks/pre-commit` (add the check)

---

### Story US-C5.3 — `human help getting-started` interactive CLI

**As** a user already in their terminal who doesn't want to switch
            to a browser to read the docs,
**I want** `human help getting-started` to walk me through the
            same content as `getting-started.md` interactively,
            with "press Enter to continue" prompts so I can read at
            my own pace,
**So that** I get pointer-by-pointer guidance without context
            switching out of the CLI.

**Acceptance criteria:**

1. New CLI sub-command `human help getting-started` registered
   through the existing help-command dispatcher.
2. Content comes from the SAME source as `getting-started.md` —
   `src/cli_commands.c` parses the markdown at runtime and chunks
   it by `##` headings, displaying one section per "press Enter"
   step. This means the markdown is the single source of truth;
   no copy-paste duplication.
3. Each section terminates with `[press Enter to continue, q to
   quit]`. Quit returns to the shell cleanly.
4. ANSI rendering: headings bold, code blocks dimmed, links
   underlined with the URL inline. No syntax highlighting beyond
   that (avoids dragging in a markdown-to-ANSI library).
5. Test: invoke the command with stdin scripted (press Enter
   through all sections), assert the rendered output contains every
   section's content. Quit-mid-flow returns 0.

**Files expected to change:**

- `src/cli_commands.c` — `help getting-started` dispatch (~50 LoC)
- `src/cli/help_getting_started.c` (NEW, ~250 LoC — markdown
  chunker + ANSI renderer)
- `tests/test_cli_help_getting_started.c` (NEW, ~300 LoC)

---

### Story US-C5.4 — `human help troubleshoot` quick-help

**As** a user whose `human doctor` just printed a FAIL,
**I want** `human help troubleshoot` to give me a flat list of the
            common failure modes with one-line fixes,
**So that** I can paste the matching FAIL message into search-and-
            fix mode instead of grepping the source.

**Acceptance criteria:**

1. New CLI sub-command `human help troubleshoot`.
2. Content lives in `docs/guides/troubleshoot.md` (NEW), structured
   as a flat list:
   ```
   ## chatdb_readable FAIL: permission denied
   You need to grant Full Disk Access to `human`.
   Open: System Settings → Privacy & Security → Full Disk Access
   then enable "human".
   ```
3. Every FAIL reason string from Sprint 50 §C3 checks has a
   matching section here. Pinned by a test:
   `tests/test_troubleshoot_doc_coverage.c` enumerates all FAIL
   reason strings from `src/doctor/check_*.c` and asserts each
   appears as a heading in `troubleshoot.md`.
4. `human help troubleshoot <reason>` jumps straight to the
   matching section (fuzzy match on the reason string).
5. Markdown-to-ANSI renderer reused from US-C5.3.

**Files expected to change:**

- `docs/guides/troubleshoot.md` (NEW, ~300 lines markdown)
- `src/cli_commands.c` — dispatch (~30 LoC)
- `tests/test_troubleshoot_doc_coverage.c` (NEW, ~200 LoC)

---

### Story US-C5.5 — Link-rot + freshness CI gate

**As** the docs surface area,
**I want** CI to fail when any `docs/guides/*.md` contains a stale
            internal link OR a code block whose first line doesn't
            shell-execute cleanly under `set -n` (syntax-check
            only),
**So that** the docs don't decay into a mausoleum of broken links
            and snippets that haven't worked since v0.4.

**Acceptance criteria:**

1. New script `scripts/check-docs-links.sh` (NEW) — walks every
   markdown file under `docs/`, extracts every `](path)` relative
   link, asserts the target exists.
2. New script `scripts/check-docs-code-blocks.sh` (NEW) — extracts
   every ``` ```bash ``` block, pipes each to `bash -n` (parse
   only, do NOT execute) and asserts the parse succeeds.
3. Both scripts wired into the existing `scripts/doc-fleet.sh`
   docs-gate workflow (CI runs it; no new GitHub Actions surface).
4. Pre-commit hook: if any `docs/**/*.md` is staged, runs both
   scripts on the staged set.
5. Tests: feed a known-broken link and a known-broken bash block
   through each script and assert the exit code is nonzero with
   the specific failure named.

**Files expected to change:**

- `scripts/check-docs-links.sh` (NEW, ~80 lines bash)
- `scripts/check-docs-code-blocks.sh` (NEW, ~60 lines bash)
- `scripts/doc-fleet.sh` (call both new scripts)
- `.githooks/pre-commit` (add the conditional check)
- `tests/test_docs_link_rot.sh` (NEW, ~150 lines bash test)

---

## Definition of Done (sprint-level)

1. **getting-started.md ≤ 400 lines** AND walks a fresh user from
   "I see the README" to "I sent a test message" in ≤ 5 minutes
   measured against the wizard's actual timing. Verifier records
   the timing on a fresh `~/.human/` checkout.
2. **README's first h2 is `## Quick start`** AND the four expected
   bullet items are present (US-C5.2 script enforces).
3. **`human help getting-started` parses the canonical markdown at
   runtime** — no duplicated copy of the content lives in C source
   strings. Pinned by a test that grep's `src/cli/help_getting_
   started.c` for any of the headings from getting-started.md and
   asserts zero hits.
4. **Link-rot + code-block CI gate is green** on every doc in
   `docs/guides/`.
5. **Aspect-panel UX review (not security this time)** — pre-merge
   panel weighted toward `correctness` and `style` (style here
   means doc readability, not code style).

## Risk register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| getting-started.md balloons to 800+ lines during review | High | Medium | 400-line hard cap enforced by `scripts/check-docs-length.sh`. PRs exceeding cap fail closed. |
| Screenshots go stale when onboarding wizard changes | High | Medium | Screenshot regeneration script: `scripts/regen-onboarding-screenshots.sh` runs the wizard against a fake terminal capture, produces deterministic PNGs. Pre-commit hook fires if `src/onboard/step_*.c` is staged and screenshots aren't. |
| README rewrite removes someone's favorite philosophy paragraph | Medium | Low | The philosophy content MOVES to `docs/why.md`, not deleted. The rewrite PR includes the diff showing all displaced content. |
| `human help getting-started` markdown parser breaks on edge cases | Low | Medium | Use a known-good minimal markdown parser (the one in `src/cli/markdown_help.c` if it exists, or borrow from `src/util/markdown_parse.c`) — DO NOT roll our own. |
| Troubleshoot doc lags behind doctor's actual FAIL strings | High | High (broken UX exactly when user needs help) | US-C5.4 §3 — the test enumerates doctor FAIL strings and asserts each is documented. A new doctor check that doesn't add a troubleshoot section fails CI. |

## Anti-goals (re-stated from backlog §C5)

- **Don't write a tutorial that recreates the onboarding wizard.**
  The wizard IS the tutorial; the doc points at it.
- **No standalone tutorial that diverges from the wizard.** Single
  source of truth: the wizard's actual steps.

## Cross-sprint dependencies

- **Hard:** C1, C2, C3 must be implemented before C5 ships
  (getting-started.md references all of them by name).
- **Soft:** C4 (telemetry) — if it ships before C5, the
  getting-started doc mentions the opt-in prompt; if it ships
  after, an `## Updates` section gets added later.
