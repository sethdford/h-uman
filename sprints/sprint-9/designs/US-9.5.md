# Design for US-9.5: Website install one-liner + verified badge

**Sprint:** 9 (Distribution)
**Risk tier:** Low (website only; no C, no security boundary)
**Depends on:** US-9.1 (Homebrew formula with real tap name + real SHA256s)
**Out of scope:** Analytics, A/B testing, demo video — per story.

---

## Approach

Add a single "Get started" section to `website/src/pages/index.astro` (no
new route — index is the marketing landing page and gets the most eyes).
The section renders:

1. A copy-paste `<code>` block with the canonical one-liner.
2. A small "verified on macOS arm64" badge whose state is computed at
   Astro **build time** from a static JSON file checked into the repo
   and updated by the `release.yml` workflow (the same workflow that
   builds the artifacts referenced by the formula).

The design's load-bearing choice is **a single source of truth for the
install command and the verification state**, so the displayed command
cannot drift from the formula. We introduce one file:

```
website/src/data/install.json
```

with shape:

```json
{
  "tap": "humanlabs/human",
  "package": "humanlabs/human/human",
  "version": "0.5.0",
  "command": "brew tap humanlabs/human && brew install humanlabs/human/human",
  "verified": {
    "macos_arm64": {
      "ok": true,
      "release_tag": "v0.5.0",
      "checked_at": "2026-05-17T00:00:00Z",
      "ci_run_url": "https://github.com/sethdford/h-uman/actions/runs/..."
    }
  }
}
```

The Astro page imports this JSON statically; both the rendered command
string and the badge text bind to fields in this object. The test seam
(below) asserts that `install.json.command` equals the literal command
that `Formula/human.rb` would install for, derived by reading the
`.rb` file at test time and parsing the tap path. This is what
catches DRIFT, not "command is non-empty".

### Why not fetch at runtime / fetch GitHub Releases?

AC-9.5.2 ("displayed version updates without manual deploy") could be
satisfied either by (a) client-side fetch of GitHub Releases at page
load, or (b) build-time fetch baked into Astro's `prebuild`. Choice
(b):

- No CORS concerns, no client JS that can fail behind a corp proxy.
- No empty-state flash while fetch resolves.
- The version updates on every CI release build (release.yml runs the
  website build), so "without manual deploy" is honored.
- Privacy thesis (CLAUDE.md): no third-party fetch from user browsers.

`release.yml` will write `install.json` as a build step before the
website build job runs (one new ~10-line shell step). Implementer
should add this in US-9.1 (formula) or US-9.5 (this story) — see
"Sequencing" below.

### Why a section on index, not a new `/install` page?

AC-9.5.1 says "home page OR `/install` page". The marketing site has
exactly one content page (`index.astro`) plus brand/design utility
pages. Adding a route just for the install command splits attention
without an SEO or UX win. If we later add docs (Starlight is already a
dep), `/install` can be the docs landing — but for now, in-page.

---

## Files to modify

| File                                                | Change                                                                                 | Est LOC |
| --------------------------------------------------- | -------------------------------------------------------------------------------------- | ------- |
| `website/src/data/install.json`                     | NEW — single source of truth: tap, package, version, command, verified state          | +18     |
| `website/src/components/InstallSection.astro`       | NEW — renders code block + verified badge; imports `install.json` statically          | +90     |
| `website/src/pages/index.astro`                     | Import + place `<InstallSection />` above the bloat list section                       | +5      |
| `website/src/styles/global.css`                     | Add 2 design-token-only rules for the install code block + badge (no raw hex/px)      | +25     |
| `website/scripts/check-install-matches-formula.mjs` | NEW — node script: parse `Formula/human.rb`, parse `install.json`, exit 1 on mismatch | +60     |
| `website/tests/install-section.spec.ts`             | NEW — vitest; asserts JSON contents + invokes the check script via `spawnSync`        | +70     |
| `website/package.json`                              | Add `"test"` script wiring vitest; add `vitest` devDep                                 | +6      |
| `.github/workflows/ci.yml`                          | Add `pnpm --filter website test` to the website job (or equivalent)                   | +3      |
| `.github/workflows/release.yml`                     | After bottling, write CI-verified state into `install.json` and commit OR write artifact | +15   |

**Total est:** ~290 LOC, mostly JSON + one Astro component + one Node check script.

No existing tests file exists at `website/tests/`; this story creates the
directory. That's why `website/package.json` needs a `"test"` script.

---

## Implementation steps (for the implementer agent)

1. **Skeleton, no behavior.** Create `website/src/data/install.json` with
   the literal values above. Create empty `InstallSection.astro` that
   renders just `<code>{installData.command}</code>` and the version.
   Wire it into `index.astro`. `pnpm build` in `website/` must still
   exit 0. No styling yet.

2. **Add the drift check script first (TDD-shaped).** Write
   `website/scripts/check-install-matches-formula.mjs`. It must:
   - Read `../Formula/human.rb` as text.
   - Extract `version "X.Y.Z"` via regex → `formulaVersion`.
   - Derive the expected one-liner from a known template plus the tap
     name. The tap name source: read it from `install.json` and from
     a comment-pinned line in the formula (e.g.
     `# tap: humanlabs/human` added in step 5) — if the comment is
     missing, exit 1 with a clear error.
   - Compare `installData.version === formulaVersion`.
   - Compare `installData.command === expectedCommand`.
   - Print a diff on mismatch and exit 1.

3. **Add the vitest spec.** `website/tests/install-section.spec.ts`:
   - Test A: `install.json` parses; required fields are non-empty
     strings; `verified.macos_arm64.ok` is a boolean.
   - Test B (the drift test, the load-bearing one):
     `spawnSync('node', ['scripts/check-install-matches-formula.mjs'])`
     completes with status 0. **This is the test that catches drift** —
     if either file changes without the other, this test fails. Per
     `.claude/rules/tests-that-pin-bugs.md`, the assertion is on the
     dangerous outcome (mismatch → drift → user runs wrong command),
     not on "command is some string". Use `spawnSync` from
     `node:child_process` with arg-array form (no shell), not a
     shell-string variant — keeps the test deterministic on Windows
     CI runners and avoids any shell-quoting surprises.
   - Test C: regression for AC-9.5.1 — assert the exact spec string
     `brew tap humanlabs/human && brew install humanlabs/human/human`
     is the value of `install.json.command`. This pins the AC literal.
   - Test D: assert no hard-coded semver string appears in the
     component source — `expect(componentSource).not.toMatch(/['"]\d+\.\d+\.\d+['"]/)`.
     AC-9.5.2 requires version to come from `install.json`, not be
     baked into the template.

4. **Implement the component visuals.** Add Phosphor icon for the
   verified badge (green check when `verified.macos_arm64.ok`, neutral
   info icon otherwise). Use only `--hu-*` tokens for colors, spacing,
   radii. The "verified" badge displays the release tag and a "checked
   on YYYY-MM-DD" line. The whole section uses
   `--hu-surface-container` for the code block and the project's
   existing Avenir font stack.

5. **Pin the tap name in the formula.** Add a comment line to
   `Formula/human.rb` near the top: `# tap: humanlabs/human`. The
   check script reads this. (Implementer must coordinate with US-9.1.)

6. **Wire the release pipeline.** In `release.yml`, after the formula
   SHA256s are updated, run a step that writes
   `verified.macos_arm64.ok=true`, `release_tag=$GITHUB_REF_NAME`,
   `checked_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)`,
   `ci_run_url=$GITHUB_SERVER_URL/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID`
   to `install.json` and commits it back to `main`. If
   `check-formula-install.sh` (a separate US-9.1 deliverable) fails,
   set `ok=false` and still commit (so the badge tells the truth).

7. **Run /verify** with the contract:
   - `pnpm --filter website build` exits 0.
   - `pnpm --filter website test` exits 0.
   - Axe a11y check passes on a built artifact (`ci.yml` axe job).
   - Manual visual check that no raw hex / pixel values were
     introduced (grep `website/src/components/InstallSection.astro`
     and the CSS rules for `#` followed by hex chars, and for `px`).

---

## Risks

- **Drift between displayed command and formula (HIGH probability if
  unguarded, SMALL impact per occurrence — user gets a 404 from
  `brew install` until they retap).** Mitigation: the
  `check-install-matches-formula.mjs` script runs in CI on every PR
  that touches `website/` OR `Formula/human.rb`. The vitest spec is
  the test seam.

- **Hard-coded version "0.5.0" sneaks in (MEDIUM / SMALL).** The bug
  pattern is a forgetful implementer copying `0.5.0` into the Astro
  template. Mitigation: Test D (above) is a regex guard against any
  semver-shaped literal in the component source.

- **`install.json` stale after release (MEDIUM / SMALL — only
  user-visible if release.yml step is forgotten).** Mitigation: add
  a "version freshness" sub-check: vitest reads the latest GitHub
  release tag (offline-skippable; gated by `process.env.CI`) and
  warns if `install.json.version` lags by more than one tag. WARN,
  not FAIL — we don't want stale releases to red the build, only to
  surface to the on-call.

- **Badge falsely shows "verified" (LOW / MEDIUM).** A user trusts
  the badge and gets a broken install. Mitigation: the
  `verified.macos_arm64.ok` field is ONLY written by the
  `release.yml` workflow after `check-formula-install.sh` (a US-9.1
  deliverable) actually succeeded. We do not write `ok=true` from
  any human-editable path. Add a CODEOWNERS rule that flags
  manual edits to `install.json` for review.

- **Backward compatibility (LOW / SMALL).** No existing callers; new
  component, new JSON file. The only existing-surface change is
  adding an import to `index.astro`. Mitigation: Astro build catches
  any import error at compile time.

- **Observability (LOW).** If the install one-liner is broken in
  production, we want to know fast. The badge IS the observability
  surface for users; for the team, `release.yml` posts to Slack on
  any step failure (existing wiring). No new alerting needed.

- **A11y (LOW / SMALL).** Code block contrast and badge color-only
  signaling are the typical axe traps. Mitigation: the badge uses
  icon + text + color (never color alone); the code block uses
  `--hu-surface-container-high` for ≥4.5:1 contrast with body text.

---

## Test strategy

- **`website/tests/install-section.spec.ts`** (vitest) — 4 tests:
  1. `install.json` schema sanity.
  2. **Drift detector** — spawns `check-install-matches-formula.mjs`,
     asserts status 0. (Load-bearing per anti-drift requirement.)
  3. Exact AC literal: `install.json.command ===
     'brew tap humanlabs/human && brew install humanlabs/human/human'`.
  4. No hard-coded semver in component source.

- **`website/scripts/check-install-matches-formula.mjs`** invoked by
  vitest AND by a new CI step `website-install-drift` in `ci.yml`.

- **Axe accessibility** (existing CI job) covers AC-9.5.5.

- **No new C tests.** This story is website-only.

### Anti-pattern checks (`.claude/rules/tests-that-pin-bugs.md`)

The drift test is phrased as the dangerous-outcome assertion:

```ts
// FAIL = user would copy a command that does not match the published formula
expect(driftCheckStatus, "install command must match Formula/human.rb")
  .toBe(0);
```

NOT as:

```ts
// pins-a-bug shape — passes even if both files are wrong in the same way
expect(installData.command).toBeTruthy();
expect(installData.command.length).toBeGreaterThan(10);
```

The test name `install_command_matches_formula_or_drift_is_detected`
is a claim, not a label — if the formula's tap name changes and
`install.json` doesn't, the test fails by design.

---

## Acceptance criteria mapping

| AC | How satisfied | Where verified |
| --- | --- | --- |
| AC-9.5.1 — exact one-liner present | `install.json.command` is the literal; `InstallSection.astro` renders it | vitest Test C |
| AC-9.5.2 — version dynamic, no hard-coded `0.5.0` | `release.yml` writes `install.json.version` on every release; component reads from JSON | vitest Test D + drift test |
| AC-9.5.3 — Astro/TS build clean | `pnpm --filter website build` is the gate | CI `ci.yml` website job |
| AC-9.5.4 — design tokens only, Phosphor icons, no raw hex/px | CSS rules use only `--hu-*`; component uses existing Phosphor import; lint grep in PR review | manual visual + grep in implementer step 7 |
| AC-9.5.5 — zero new axe violations | Existing CI axe job runs against built site | CI `ci.yml` axe job |

---

## Sequencing with US-9.1

This story has a hard dependency on US-9.1 for the canonical tap name
and a soft dependency for the `check-formula-install.sh` CI gate that
sets `verified.macos_arm64.ok=true`. Implementer should:

1. Confirm tap name with US-9.1 owner before step 1 (the story already
   names a blocking open question on tap org — see stories.md line 186).
2. Land steps 1–4 (skeleton + drift check + vitest + visuals) in this
   story's PR; the badge can ship in `ok=false` (or `ok=null` =
   "pending verification") state until US-9.1 lands.
3. Land step 6 (release.yml wiring) in a follow-up PR after US-9.1
   merges. Out of scope to block this story on release.yml changes.

---

## DoD checklist (matches stories.md line 132)

- [ ] Website CI green: `pnpm --filter website build` + axe
- [ ] Install command on page matches real formula (vitest drift test)
- [ ] Version is dynamic (no hard-coded `0.5.0` in component source)
- [ ] `/verify` ran and returned PASS
- [ ] No raw hex / no raw px in new CSS or component
