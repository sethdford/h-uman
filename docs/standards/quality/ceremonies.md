# Quality Ceremonies

Recurring checkpoints that prevent standards drift. Each ceremony has a cadence, an owner, and a checklist. Skip none.

**Cross-references:** [governance.md](governance.md), [code-review.md](code-review.md)

---

## Weekly Drift Audit (15 min)

**Cadence:** Every Monday
**Owner:** Whoever touched the most code that week (or the project lead)

### Checklist

1. Build and run the full test suite:
   ```bash
   cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
   ./build/human_tests
   ```
2. Check for uncommitted work:
   ```bash
   git stash list
   git status
   ```
3. Review `docs/plans/` for open implementation plans and unfinished tasks
4. Scan for design token drift:
   ```bash
   bash scripts/lint-raw-colors.sh --all
   npm run lint:tokens --prefix ui
   ```
5. Check for orphaned documentation:
   ```bash
   ./scripts/check-doc-index.sh
   ```
6. Verify standards propagation is in sync:
   ```bash
   ./scripts/check-standards-drift.sh
   ```
7. Log results as a commit message or in the remediation PR description

### What "Pass" Looks Like

- Zero test failures, zero ASan errors, build succeeds
- No uncommitted changes on `main`
- All plan tasks either complete, explicitly deferred, or tracked
- Zero raw hex colors outside token definition files
- Zero orphaned `.md` files
- Agent files (`CLAUDE.md`, `AGENTS.md`) reference current standards

---

## PR Completion Gate (per PR)

**Cadence:** Every pull request, before requesting review
**Owner:** PR author

### Checklist

- [ ] Build succeeds with zero warnings
- [ ] All tests pass with zero ASan errors
- [ ] Changes scoped to a single concern (not a grab bag)
- [ ] Standards alignment verified (read the applicable `docs/standards/` doc)
- [ ] Agent files updated if commands, architecture, or standards changed
- [ ] No orphaned docs -- new `.md` files are indexed in the appropriate README
- [ ] Design tokens used -- no raw hex colors, pixel spacing, or pixel radii
- [ ] No dead code, debug artifacts, or stale `TODO` hacks
- [ ] `HU_IS_TEST` guards present for any new side effects
- [ ] Every allocation freed
- [ ] PR description follows conventional commit format

### When to Escalate

If the PR introduces a new pattern that no standard covers, do not merge until:

1. The pattern is documented in a new or updated standard in `docs/standards/`
2. Agent files are updated to reference the new standard
3. The PR description notes the new standard

---

## Release Gate (per release)

**Cadence:** Before tagging any release
**Owner:** Release owner

### Checklist

1. Run the full drift audit (weekly checklist above)
2. Verify all plan tasks for this release are complete or explicitly deferred
3. Confirm CHANGELOG is updated with this release's changes
4. Build release binary and verify size:
   ```bash
   cmake -B build-release -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON
   cmake --build build-release -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
   ls -la build-release/human
   ```
5. Run startup time and RSS benchmarks -- no regressions from baseline
6. Tag the release only after all checks pass

### Release Blockers

These items block a release tag:

- Any test failure or ASan error
- Binary size regression without justification
- Orphaned documentation
- Open plan tasks marked as "required for this release"
- Missing CHANGELOG entry

---

## Quarterly Award-Criteria Audit (60 min)

**Cadence:** First Monday of each quarter (Q1 Jan, Q2 Apr, Q3 Jul, Q4 Oct)
**Owner:** Project lead or design lead

### Checklist

1. Review the quality scorecard (`docs/quality-scorecard.md`) against the 63+/70 target:
   ```bash
   bash scripts/quality-score-report.sh
   ```
2. Run the competitive benchmark and compare against named competitors:
   ```bash
   bash scripts/benchmark-competitive.sh --markdown
   ```
3. Audit each surface against the award-criteria checklists in `docs/standards/quality/award-criteria.md`:
   - **Website**: Awwwards checklist (Lighthouse, LCP, WebGL, scroll narrative, View Transitions)
   - **Dashboard**: Webby checklist (performance, UX, functionality)
   - **iOS**: Apple Design Award checklist (Dynamic Type, VoiceOver, haptics, App Intents)
   - **Android**: Google Play Best App checklist (Material 3, Compose, predictive back, widget)
4. Update per-surface scores in `docs/quality-scorecard.md` with evidence
5. Identify the top 3 gaps per surface and create implementation plans
6. Review the Design Innovation Pipeline in `docs/standards/design/design-strategy.md`:
   - Did we adopt the planned quarterly feature?
   - Queue the next quarter's feature adoption
7. Check award submission readiness in `docs/competitive-benchmarks.md`
8. Log results and create remediation issues for any regressions

### What "Pass" Looks Like

- All surface scores updated with evidence
- No surface has regressed from the previous quarter
- At least one surface has improved by 2+ points
- Next quarter's innovation pipeline feature is identified
- Award submission blockers are documented with remediation plans

---

## Ceremony Hygiene

- **Never skip a ceremony.** If the weekly audit finds nothing, log "clean" and move on. The act of checking is the value.
- **Automate what you can.** Scripts in `scripts/` exist to make ceremonies fast. If a manual check takes more than 30 seconds, write a script.
- **Drift is silent.** The whole point of ceremonies is catching problems that do not announce themselves. Treat the weekly audit like a health check, not a punishment.
