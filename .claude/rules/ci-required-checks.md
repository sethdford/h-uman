# CI Required Checks — SOTA Gate Policy

> Authored 2026-05-17 alongside the ci-rot-cleanup pass that turned 18
> failing workflows into 16+ green ones.

## Why this rule exists

`main` had **no branch protection** when the rot accumulated. Without
required checks, CI failures became advisory — and advisory failures
get ignored, then become normal, then become invisible. The
ci-queue-triage rule depends on having required checks to defend.

This rule defines which workflows are trustworthy enough to gate on,
which are advisory, and which are too flaky/expensive to enforce.

## Tier 1 — Required for merge (CORE)

These workflows must pass on every PR before merge. They exercise the
build matrix and core test surface across configurations:

| Workflow | Why required |
|---|---|
| `build-and-test (ubuntu-latest)` | Full-features test surface (10858 tests, HU_ENABLE_ML=ON) |
| `build-and-test (macos-latest)` | macOS clang catches different warnings than Linux GCC |
| `minimal-build` | Validates the no-feature-flags baseline (~9000 tests) |
| `cross-arm64` | Cross-compile sanity for aarch64 (Docker/cloud) |
| `feature-flags (no-sqlite)` | SQLite-OFF + ALL_CHANNELS-ON path |
| `feature-flags (no-skills)` | Skills-OFF path |
| `feature-flags (kitchen-sink)` | Persona + Skills + SQLite enabled |
| `feature-flags (llamacpp-on)` | M3 Bridge A compile path |
| `integration-tests` | Live-gateway integration suite |
| `static-analysis` | clang-tidy + warnings-as-errors |
| `local-check` | The `scripts/check-*.sh` family (terminology, frontmatter, index, links, drift) |
| `docs` | Generated docs + doc-fleet (frontmatter + markdown links + drift) |
| `ui` | UI vitest suite |
| `e2e-smoke` | Minimal Playwright path |
| `nix` | Nix flake build reproducibility |
| `build-deb` | Debian packaging |
| `website` | Astro marketing site build |
| `design-tokens` | Token validator + drift |
| `completions` | Shell completions regen |
| `docker` | Container image build |
| `build-android` | Android shared lib build |
| `ui-e2e` | Dashboard Playwright suite incl. per-view axe scan (promoted 2026-09-27, see History) |

## Tier 2 — Advisory (must run, may fail)

These provide signal but are too flaky or environment-dependent to
gate. CI runs them on every PR but their failure does not block merge:

| Workflow | Why advisory |
|---|---|
| `visual-regression` | Snapshot drift on font rendering / pixel diff |
| `lighthouse` | Performance scores fluctuate ±5% run-to-run |
| `lighthouse-dashboard` | Same as above |
| `coverage` | Codecov tokens / network occasionally flake |
| `dead-strip-ratchet` | New 2026-09-26. NOT flaky and not environment-dependent — it is deterministic and fails only on a real baseline regression. Advisory purely because it is new: promote to Tier 1 under the ≥50-PR bar below. Until then it is visible-but-not-blocking, which is strictly more than the zero CI coverage the dead-strip ratchet had before it. |

## Tier 3 — Main-only / on-demand

These intentionally run only on main pushes or workflow_dispatch:

| Workflow | Trigger |
|---|---|
| `benchmark` | `main` push only (timing measurements need stable baseline) |
| `fuzz` | `main` push only (30s libFuzzer runs are time-budgeted) |
| `ollama-integration` | `main` push only (needs cached ollama image) |
| `quality-score` | `main` push only (depends on build-and-test + ui) |
| `quality-score-scheduled` | `schedule` / `workflow_dispatch` |

## Tier 4 — Release-only

| Workflow | Trigger |
|---|---|
| `release-size` | Release builds (binary-size guard) |
| `build-ios`, `build-macos` | Release builds (signing cost) |

## Enforcement

Branch protection on `main` MUST require all Tier-1 workflows. Set via
the GitHub UI or via `gh api`:

```bash
gh api -X PUT repos/sethdford/h-uman/branches/main/protection \
  --input - <<'JSON'
{
  "required_status_checks": {
    "strict": true,
    "checks": [
      {"context": "build-and-test (ubuntu-latest)"},
      {"context": "build-and-test (macos-latest)"},
      {"context": "minimal-build"},
      {"context": "cross-arm64"},
      {"context": "feature-flags (no-sqlite, -DHU_ENABLE_SQLITE=OFF -DHU_ENABLE_ALL_CHANNELS=ON)"},
      {"context": "feature-flags (no-skills, -DHU_ENABLE_SQLITE=ON -DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_SKILLS=OFF)"},
      {"context": "feature-flags (kitchen-sink, -DHU_ENABLE_SQLITE=ON -DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_PERSON..."},
      {"context": "feature-flags (llamacpp-on, -DHU_ENABLE_SQLITE=ON -DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_LLAMACP..."},
      {"context": "integration-tests"},
      {"context": "static-analysis"},
      {"context": "local-check"},
      {"context": "docs"},
      {"context": "ui"},
      {"context": "e2e-smoke"},
      {"context": "nix"},
      {"context": "build-deb"},
      {"context": "website"},
      {"context": "design-tokens"},
      {"context": "completions"},
      {"context": "docker"},
      {"context": "build-android"},
      {"context": "ui-e2e"}
    ]
  },
  "enforce_admins": false,
  "required_pull_request_reviews": null,
  "restrictions": null,
  "allow_force_pushes": false,
  "allow_deletions": false
}
JSON
```

**Context names must match the check run byte for byte.** GitHub truncates
check-run names to 100 characters with a trailing `...`, so the two long
`feature-flags` matrix rows above end in a literal `...`. A full, untruncated
name never matches, stays pending forever, and blocks every PR. When a matrix
row's args change, re-read the real names before editing protection:
`gh api "repos/sethdford/h-uman/commits/<sha>/check-runs?per_page=100" --paginate --jq '.check_runs[].name'`
(use the head SHA of a *successful* main run; cancelled runs report nothing).

The `enforce_admins: false` and `required_pull_request_reviews: null`
keep the gate functional for a solo developer while still preventing
merges with red CI. Tighten when team grows.

## When a Tier-1 check is genuinely broken on main

Per `ci-queue-triage.md`: if ≥3 PRs fail on the same Tier-1 check,
treat as systemic. The bootstrap-fix escape hatch:

1. Verify on the fix PR's own CI that the previously-broken check now
   passes after the fix.
2. Admin-merge with `gh pr merge --admin --reason "ci-meta-bug-bootstrap-fix"`.
3. Re-trigger the queue: `gh pr update-branch <N>` for each blocked PR.

This is the ONLY legitimate use of `--admin` against a red gate.

## When to promote/demote a workflow

- **Promote Advisory → Required** when it has run on ≥50 PRs without a
  false-positive failure (clean track record).
- **Demote Required → Advisory** if it has caused ≥3 false-positive
  red gates in a quarter (genuine flake).

Always document promotion/demotion in this file's history.

## History

- **2026-09-27: `ui-e2e` promoted Advisory → Required** (user decision, ahead
  of the ≥50-PR bar). All 3 `ui-e2e` failures in the preceding 13 completed
  `main` runs (36285820022, 36257002769, 36253284792) were the per-view axe
  scan sampling text mid-fade (Chat ×3, Overview, Voice), not WS proxy churn.
  PR #494 made that scan wait for data + animations and pinned the
  remaining real violations in `KNOWN_VIOLATIONS`. Demote under the rule
  above if it flakes again.
- **2026-09-27: branch protection brought in line with Tier 1.** `main` had
  only 5 required checks; it now requires all 22 Tier-1 contexts above (names
  verified against check runs on a3975fed5; every one succeeded in 7/7 recent
  main runs except `ui-e2e`, 6/7, the flake fixed by #494). Force pushes and
  deletion of `main` disabled the same day.
