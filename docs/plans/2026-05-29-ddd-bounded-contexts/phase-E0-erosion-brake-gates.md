# Phase E0 — Erosion-Brake Gates (Stop the Bleeding)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Install the four **enforcement gates** that make the program owner's
"done bar" measurable and permanent, each as a **ratchet at today's baseline** so
the build stays green now and only tightens. This phase fixes the root cause of
v1's slip: T1 eroded (113→114 includers, 33→41 callers) because no ratchet fenced
the growth. Nothing structural moves here — only guards land.

**Architecture:** Four bash check scripts in the established
`scripts/check-*.sh` family, each wired into `.githooks/pre-commit` and (per
`.claude/rules/ci-required-checks.md`) the `local-check` CI job. Every script
copies the proven `scripts/check-agent-core-boundary.sh` idiom: capture a
`BASELINE` constant, fail only on growth, print the offending paths, and tell the
committer how to lower the baseline when a phase legitimately drops the count.

**Tech Stack:** Bash, `.githooks/pre-commit`, `wc -l`, `find`, optional PMD-CPD
(or a self-contained token-hash duplication scanner).

**Why ratchets, not absolutes (the v1 lesson):** an absolute "no file > 800 LOC"
rule fails the build today (17 files exceed 2,000). A ratchet pins the *current
max* and forbids *growth*; each later phase lowers the ceiling as it shrinks its
target. Green today, monotonically better forever. See
`.claude/rules/sqlite-includer-ratchet.md` for the canonical example this models.

---

## Baselines (measured 2026-05-31 — record these verbatim in the scripts)

| Gate | Baseline | Capture command |
|---|---|---|
| File-size max | `14723` (`src/daemon.c`) | `find src -name '*.c' \| xargs wc -l \| sort -rn \| sed -n '2p'` |
| Root `.c` count | `101` | `find src -maxdepth 1 -name '*.c' \| wc -l` |
| Clone blocks | (run the scanner once; record its number) | see Task 3 |
| Untested sources | (current `check-untested.sh` count) | `bash scripts/check-untested.sh; echo $?` |

---

## File Structure

- Create: `scripts/check-file-size-ceiling.sh`, `.claude/rules/file-size-ceiling.md`
- Create: `scripts/check-no-new-root-files.sh`, `.claude/rules/no-new-root-files.md`
- Create: `scripts/check-clone-ratchet.sh`, `.claude/rules/clone-ratchet.md`
- Modify: `scripts/check-untested.sh` (or wrap) for the coverage-preserving gate
- Modify: `.githooks/pre-commit` — wire the three new checks
- Modify: the CI `local-check` job to invoke the three new scripts

---

### Task 1: File-size ceiling ratchet (highest structural value)

**Files:** Create `scripts/check-file-size-ceiling.sh`, `.claude/rules/file-size-ceiling.md`.

- [ ] **Step 1: Capture the baseline.** Run the capture command above; confirm `14723`. This is `MAX_BASELINE`. The aspirational `TARGET_CEILING=800` is documented but NOT enforced yet (it would fail 65 files).

- [ ] **Step 2: Write the ratchet** — fail when ANY `src/**/*.c` exceeds `MAX_BASELINE`, i.e. no file may grow past today's worst. As each phase shrinks the worst offender, lower `MAX_BASELINE`.

```bash
#!/usr/bin/env bash
# check-file-size-ceiling.sh — no src/*.c may exceed the current max-LOC ratchet.
# Ratchet: lower MAX_BASELINE whenever the largest file shrinks (E2 drives daemon.c down).
# Aspirational target documented in .claude/rules/file-size-ceiling.md: 800 LOC.
set -euo pipefail
MAX_BASELINE=14723   # src/daemon.c, measured 2026-05-31. Lower as god-files are carved.
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
worst=$(find src -name '*.c' | xargs wc -l 2>/dev/null | awk '$2!="total"' | sort -rn | sed -n '1p')
worst_loc=$(echo "$worst" | awk '{print $1}'); worst_file=$(echo "$worst" | awk '{print $2}')
echo "largest src/*.c: $worst_file = $worst_loc LOC (ceiling $MAX_BASELINE)"
if [ "$worst_loc" -gt "$MAX_BASELINE" ]; then
  echo "FAIL: a file grew past the size ratchet. Split it, or it cannot land." >&2
  find src -name '*.c' | xargs wc -l 2>/dev/null | awk -v b="$MAX_BASELINE" '$2!="total" && $1>b {print "  "$0}' >&2
  exit 1
elif [ "$worst_loc" -lt "$MAX_BASELINE" ]; then
  echo "NOTE: largest file shrank to $worst_loc — lower MAX_BASELINE to lock the gain." >&2
fi
```

- [ ] **Step 3: Write the rule doc** (`.claude/rules/file-size-ceiling.md`) — explain the ratchet, the 800 target, and "lower the baseline when you shrink the worst file." Model the prose on `.claude/rules/sqlite-includer-ratchet.md`.
- [ ] **Step 4: Smoke-test** — `bash scripts/check-file-size-ceiling.sh; echo $?` → `0`. Add a temporary 14,800-LOC fixture file under `src/` (or bump the constant down by 1) to confirm it fails, then revert.
- [ ] **Step 5: Commit** — `chore(ddd): add file-size-ceiling ratchet (E0 gate 1)`.

---

### Task 2: No-new-root-files ratchet (locks E1's target before E1 starts)

**Files:** Create `scripts/check-no-new-root-files.sh`, `.claude/rules/no-new-root-files.md`.

- [ ] **Step 1: Baseline** — `find src -maxdepth 1 -name '*.c' | wc -l` → `101`. This is `ROOT_BASELINE`.

- [ ] **Step 2: Write the ratchet** — fail if the loose-root count grows past `101`. E1 chips lower it; the floor is `0`.

```bash
#!/usr/bin/env bash
# check-no-new-root-files.sh — the count of loose src/*.c (no bounded-context dir) may only shrink.
# Ratchet: E1 relocations lower ROOT_BASELINE; floor is 0, then flip to a hard "no new root .c" gate.
set -euo pipefail
ROOT_BASELINE=101   # measured 2026-05-31
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
n=$(find src -maxdepth 1 -name '*.c' | wc -l | tr -d ' ')
echo "loose src/*.c at root: $n (ceiling $ROOT_BASELINE)"
if [ "$n" -gt "$ROOT_BASELINE" ]; then
  echo "FAIL: a new file landed loose at src/ root. Put it in a bounded context" >&2
  echo "      (src/<context>/), not src/. See docs/standards/engineering/bounded-contexts.md." >&2
  find src -maxdepth 1 -name '*.c' | sed 's/^/  /' >&2
  exit 1
elif [ "$n" -lt "$ROOT_BASELINE" ]; then
  echo "NOTE: root count dropped to $n — lower ROOT_BASELINE to lock the gain." >&2
fi
```

- [ ] **Step 3: Rule doc + Step 4: smoke-test (add/remove a `src/zzz_tmp.c` to prove fail) + Step 5: commit** — `chore(ddd): add no-new-root-files ratchet (E0 gate 2)`.

---

### Task 3: Zero-duplication (clone) ratchet (net-new tooling)

**Decision (NEEDS SIGN-OFF — pick the scanner):**

| Option | What | Verdict |
|---|---|---|
| **A. PMD-CPD** (chosen) | Mature copy-paste detector; `cpd --language c --minimum-tokens 100`. JSON/XML report → count clone groups. | **Recommended.** Battle-tested, C-aware tokenizer, runs in CI via the `pmd` Homebrew/Docker image. Cost: one CI dependency. |
| **B. Self-contained token-hash script** | ~80-line bash/awk: normalize whitespace, sliding-window hash of N-line blocks, report collisions. | Fallback if adding PMD to CI is unwanted. Cruder (line-based, not token-based) but zero external deps. |
| **C. jscpd** | Node-based; supports C. | Pulls Node into the C CI path; only worth it if the UI CI already runs it. |

- [ ] **Step 1: Run the scanner once, record the baseline clone-group count** as `CLONE_BASELINE`. (For PMD: `cpd --language c --minimum-tokens 100 --files src --format csv | tail -n +2 | wc -l`.)
- [ ] **Step 2: Write `scripts/check-clone-ratchet.sh`** in the same shape — run the scanner, count groups, fail if `> CLONE_BASELINE`. Exempt generated files (`*_generated.c`, vendored `third_party/`).
- [ ] **Step 3: Rule doc** noting that E1–E4 *relocations* may transiently create then remove clones, so the ratchet is checked post-chip, and that genuine dedup work lowers `CLONE_BASELINE`.
- [ ] **Step 4: Smoke-test + Step 5: commit** — `chore(ddd): add clone-detection ratchet (E0 gate 3)`.

> If CI cannot take a new dependency, ship Option B now and track an upgrade to
> PMD as a follow-on chip. The ratchet *mechanism* is what matters for E0; the
> scanner can improve later.

---

### Task 4: Coverage-preserving gate (extend existing `check-untested.sh`)

`scripts/check-untested.sh` already flags sources with no test references
(sibling to `.claude/rules/test-references-production-symbol.md`). The
coverage-preserving requirement is: **a refactor chip must never move a source
out from under its test.**

- [ ] **Step 1: Confirm current behavior** — `bash scripts/check-untested.sh; echo $?`. Record the current untested count as the ratchet baseline (it should already be a ratchet; if not, add a `UNTESTED_BASELINE` constant the same way).
- [ ] **Step 2: Add a relocation-aware assertion** — when a chip `git mv`s `src/X.c`, the matching `tests/test_X.c` (per the module-name heuristic in `check-test-references.sh`) must still resolve to the new path. Extend `check-untested.sh` (or add a thin `check-coverage-preserving.sh`) so a moved source whose test reference breaks is reported as a regression.
- [ ] **Step 3: Rule doc + smoke-test + commit** — `chore(ddd): coverage-preserving ratchet over check-untested (E0 gate 4)`.

---

### Task 5: Wire all four into the hook + CI, verify green

**Files:** Modify `.githooks/pre-commit`, the CI `local-check` job.

- [ ] **Step 1: Add the three new scripts to `.githooks/pre-commit`** next to the existing `check-agent-core-boundary.sh` / `check-sqlite-includer-ratchet.sh` invocations. Fire size + root + clone on any staged `src/` change; coverage on any staged `tests/` or `src/` change.
- [ ] **Step 2: Add them to the `local-check` CI job** (`.claude/rules/ci-required-checks.md` Tier-1) so a `--no-verify` local bypass is still caught in CI.
- [ ] **Step 3: Full green check** — run all four scripts at HEAD; every one exits `0` (they're at baseline). Then run `git commit` of a trivial no-op `src/` touch to confirm the hook fires and passes.
- [ ] **Step 4: Commit** — `ci(ddd): enforce E0 erosion-brake gates in pre-commit + local-check`.

---

## Self-Review

- **Spec coverage:** all four gates land (size, root, clone, coverage), each a
  ratchet at the measured 2026-05-31 baseline, wired into hook + CI. ✓
- **No placeholders:** baselines are exact measured numbers (14723 / 101); the one
  genuine unknown (clone count) has an explicit "run once, record" step and a
  signed-off scanner choice, not a TODO. ✓
- **Behavior preservation:** E0 touches no `src/` logic — only `scripts/`,
  `.claude/rules/`, `.githooks/`, CI. Zero risk to the product binary. ✓
- **Stops the v1 erosion:** the size + root + clone ratchets fence exactly the
  growth that let T1 regress; E3 resumes on a fenced baseline. ✓
- **Ties forward:** E1 lowers `ROOT_BASELINE`; E2 lowers `MAX_BASELINE`; E3
  continues the sqlite ratchet; each gain is locked by lowering a constant. ✓
