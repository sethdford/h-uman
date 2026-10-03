---
title: Development Workflow
---

# Development Workflow

Standards for branching, commits, versioning, and releases in the human runtime.

**Cross-references:** [../quality/ceremonies.md](../quality/ceremonies.md), [../quality/code-review.md](../quality/code-review.md)

---

## Branching Strategy

Trunk-based development with short-lived feature branches.

```
main (always buildable, always passes tests)
  +-- feat/add-voice-channel      (feature)
  +-- fix/memory-leak-sqlite      (bug fix)
  +-- refactor/tool-dispatch      (refactor)
  +-- docs/add-ai-standards       (documentation)
  +-- test/provider-edge-cases    (tests)
```

### Branch Naming

```
<type>/<short-description>

Types: feat, fix, refactor, test, docs, chore, perf
```

- Lowercase, hyphen-separated
- Max 50 characters
- Descriptive enough to understand without reading the PR

```
RIGHT:  feat/imessage-typing-indicator
RIGHT:  fix/gateway-crash-on-empty-body
WRONG:  update (too vague)
WRONG:  seth/working-on-stuff (no names, no vague descriptions)
```

### Branch Lifecycle

1. Branch from `main`
2. Make changes in small, focused commits (one concern each)
3. Push and open a PR (see `docs/standards/quality/code-review.md`)
4. Merge after review approval and all checks pass
5. Delete the branch after merge

**Rules:**

- No long-lived branches (max 3 days before merge or rebase)
- No direct commits to `main` (pre-push hook enforces this)
- Rebase on `main` before merging if behind

---

## Commit Messages

Format enforced by `.githooks/commit-msg`:

```
<type>[(<scope>)]: <description>

Optional body explaining why, not what.
```

**Examples:**

```
feat(channels): add WhatsApp Business channel
fix(memory): prevent double-free in sqlite backend
refactor(tools): extract validation into shared helper
test(providers): add fallback chain edge cases
docs: add AI evaluation standards
chore: update CMake minimum to 3.20
perf(agent): reduce context assembly allocations
```

**Rules:**

- Imperative mood ("add", not "added" or "adds")
- No period at the end of the subject line
- Subject line max 72 characters
- Body wraps at 80 characters
- Types: `feat`, `fix`, `refactor`, `test`, `docs`, `chore`, `perf`, `ci`, `build`, `style`

---

## Release Process

### Release Build

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON -DHU_ENABLE_ALL_CHANNELS=ON
cmake --build build-release -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

### Release Checklist

1. All tests pass with zero ASan errors
2. Release build compiles clean
3. Binary size within the <!-- fp:budget_binary_kb -->3000 KB<!-- /fp --> budget (now <!-- fp:binary_kb -->~2760 KB<!-- /fp -->; CI fails a main build over it)
4. Startup time and RSS benchmarked -- no regressions
5. CHANGELOG updated with this release's changes
6. Run `./scripts/verify-all.sh` (full drift audit)
7. Tag the release: `git tag -a vX.Y.Z -m "Release notes"`
8. Build release artifacts: `./scripts/release.sh`

### Hotfix Process

For critical production issues:

1. Branch from `main`: `fix/critical-issue-description`
2. Fix + test (minimum: the specific bug + regression test)
3. PR with expedited review
4. Merge, tag as `vX.Y.Z`
5. Build and distribute
6. Backfill: full test suite and drift audit post-release

---

## Git Hooks

The repository ships with pre-configured hooks in `.githooks/`. Activate once per clone:

```bash
git config core.hooksPath .githooks
```

| Hook         | What it does                                                  |
| ------------ | ------------------------------------------------------------- |
| `pre-commit` | Runs format checks -- blocks commit if code is not formatted  |
| `commit-msg` | Enforces conventional commit format                           |
| `pre-push`   | Runs build + full test suite -- blocks push if any test fails |

Bypass only in emergencies: `git commit --no-verify` / `git push --no-verify`.

---

## Anti-Patterns

```
WRONG -- Long-lived feature branches that diverge from main for weeks
RIGHT -- Short-lived branches, max 3 days, rebased frequently

WRONG -- "WIP" or "fix stuff" commit messages
RIGHT -- Descriptive conventional commits that explain the change

WRONG -- Skip the release checklist because "it's a small change"
RIGHT -- Every release goes through the checklist; small changes can have big impact

WRONG -- Direct push to main to "save time"
RIGHT -- PR workflow catches issues that self-review misses
```

## Normative References

| ID            | Source                             | Version                          | Relevance                            |
| ------------- | ---------------------------------- | -------------------------------- | ------------------------------------ |
| [Accelerate]  | Forsgren, Humble, Kim — Accelerate | 1st ed. (2018)                   | DORA metrics for deployment velocity |
| [TBD]         | Trunk Based Development            | trunkbaseddevelopment.com (2023) | Short-lived branch methodology       |
| [ConvCommits] | Conventional Commits               | 1.0.0                            | Commit message format standard       |
