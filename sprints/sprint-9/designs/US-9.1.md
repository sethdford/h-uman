# Design for US-9.1: Homebrew install path (P0)

## Approach

The formula skeleton (`Formula/human.rb`) is structurally complete: it has a
pre-built-binary path (macOS arm64, Linux x86_64, Linux arm64), a `head`
build-from-source fallback, completions, man pages, and a `test do` block that
asserts `human --version` contains `"human"`. Three concrete gaps stand between
that skeleton and a one-command install:

1. **Placeholder SHA256s** — all three URLs ship `sha256
   "0000…"`. `brew install` will fail the hash check on every architecture
   until those are replaced with real digests of the artifacts produced by
   `release.yml`.
2. **No tap repo wiring** — `humanlabs/homebrew-human` does not exist as a
   live tap; the formula sits under `Formula/` in this repo. `brew install
   humanlabs/human/human` resolves only if the formula is pushed to a
   `homebrew-human` repo under the `humanlabs` org. (The org choice is
   already flagged as the only open ambiguity in `sprints/sprint-9/stories.md`
   line 186 — design assumes `humanlabs` per sprint brief and pins the
   resolution as a precondition rather than re-litigating it.)
3. **No CI smoke** — `release.yml` produces the artifacts but never verifies
   that the formula installs cleanly from them. Today, the only way to find
   out the formula is broken is a user reports it.

The cheapest design that satisfies the AC is three additive scripts plus one
new release job, with **no change to the formula's structure** beyond
parametrising the version and the three SHA256s. The artifact-producing
release job already publishes `sha256sums.txt`; we will *consume* that file
rather than recompute hashes (single source of truth — the file that ships
with the release IS the file we trust).

Bottle vs build-from-source: we are NOT publishing Homebrew bottles in this
story. A "bottle" in Homebrew is a pre-compiled tarball with a `bottle do`
block in the formula; producing one requires `brew bottle` against a built
keg and ABI-relocatable binaries. **What we have today (and what AC-9.1.2
will actually be satisfied by) is a pre-built binary downloaded via the
formula's `url` + `sha256` lines** — Homebrew calls these "pre-built
binaries" or "raw downloads"; they install without a compiler but do not
benefit from `brew install --force-bottle` semantics or `brew bottle`
delivery. The user-visible outcome is identical: no cmake invocation, no
Xcode requirement. The formula already implements this path (`bin.install
Dir["human-*"].first => "human"` at line 54). Real bottles are deferred
until US-8.4 (signing) lands, because bottling an unsigned binary makes
tampering attribution worse, not better (the bottle SHA covers the tarball,
not the inner binary, and any sandbox-applied codesigning happens
post-install).

Tap repo strategy: the `humanlabs/homebrew-human` repo exists as a thin
mirror of the formula file. The release workflow's last step (after the
GitHub release is created and `sha256sums.txt` is published) does
`git clone` of the tap repo, runs `scripts/update-formula-hashes.sh` against
its `Formula/human.rb`, commits with message
`release: bump human to v<tag>`, and pushes. A `HUMANLABS_TAP_PUSH_TOKEN`
PAT lives in repo secrets (write access to `humanlabs/homebrew-human` only —
no other repo scope). The formula in THIS repo (`Formula/human.rb`) is the
source of truth and is mirrored on every release; users never see it
directly, they see the tap copy.

CI smoke test: a new `brew-install-smoke` job runs on `macos-latest`,
**only on the `release` workflow** (not on every PR — too slow and depends
on a published release existing). It taps the just-pushed formula, runs
`brew install --build-from-source humanlabs/human/human` (the build-from-
source path is what we can verify without a published binary mid-release),
then `human --version` and `brew test humanlabs/human/human`. For the
pre-built-binary path, a second job downloads the artifact from the just-
created release, computes its SHA, and asserts it matches the formula's
SHA — proving the hash-update step worked without actually re-running
`brew install` on the live network (which is what users do).

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `Formula/human.rb` | Bump `version` to release tag; placeholder SHA256s replaced by `update-formula-hashes.sh` at release time. No structural change. | +0 / -0 (mutated by script) |
| `scripts/update-formula-hashes.sh` | NEW. Takes `--version <semver>` and `--sums <sha256sums.txt>`, edits `Formula/human.rb` in place using `sed`/`awk` to set version + three SHA256s. Idempotent. | +80 |
| `scripts/check-formula-install.sh` | NEW. Used by CI; runs `brew tap` + `brew install --build-from-source` + `brew test` + `human --version` semver-vs-tag assertion. | +60 |
| `.github/workflows/release.yml` | NEW job `update-tap` (depends on `release`): clone `humanlabs/homebrew-human`, run `update-formula-hashes.sh`, commit + push. NEW job `brew-install-smoke` (depends on `update-tap`, `macos-latest`): run `check-formula-install.sh`. | +60 |
| `sprints/sprint-9/evidence/US-9.1-smoke.txt` | Captured `brew install` output (added by implementer post-merge) | +n/a |
| `humanlabs/homebrew-human/README.md` (tap repo, out-of-tree) | One-line install command, link back to `h-uman` repo | +20 |

No changes to `CMakeLists.txt`, `include/human/*`, or any C source. Risk
surface is bounded to the formula, three scripts, and one workflow.

## Implementation steps (for the implementer agent)

1. **Confirm tap org name**: open `humanlabs/homebrew-human` as a new public
   repo if it does not exist (one-time bootstrap; coordinate with user — do
   NOT create under `sethdford/` because every release will push to it).
   Generate the `HUMANLABS_TAP_PUSH_TOKEN` PAT scoped to that repo only and
   add to this repo's Actions secrets.
2. **Write `scripts/update-formula-hashes.sh`** — pure-bash, no Python. Inputs:
   `--version v0.5.0`, `--sums sha256sums.txt`. Algorithm:
   a. Parse `sha256sums.txt` for the three target lines.
   b. Use `sed -i.bak` to rewrite the `version "..."` line and the three
      `sha256 "0000…"` placeholders. Match by URL context (don't blindly
      replace all-zero strings — too brittle).
   c. Exit non-zero if any of the three architectures was not matched.
3. **Unit-test the script** under `tests/scripts/test_update_formula_hashes.sh`
   (bash harness already exists per CI pattern). Cover: happy path, missing
   architecture, malformed sums file, idempotent re-run.
4. **Write `scripts/check-formula-install.sh`**:
   a. `brew tap humanlabs/human https://github.com/humanlabs/homebrew-human`
   b. `brew install --build-from-source humanlabs/human/human`
   c. `human --version | grep -E "^[0-9]+\.[0-9]+\.[0-9]+$"`
   d. Assert the printed version equals `$GITHUB_REF_NAME` with the leading
      `v` stripped.
   e. `brew test humanlabs/human/human` (covers AC-9.1.4).
5. **Add release workflow jobs**:
   - `update-tap` (Ubuntu, depends on `release`): downloads `sha256sums.txt`
     from the just-published release via `gh release download $GITHUB_REF_NAME
     -p sha256sums.txt`, clones the tap, runs `update-formula-hashes.sh`,
     commits, pushes using the PAT.
   - `brew-install-smoke` (macos-latest, depends on `update-tap`): runs
     `check-formula-install.sh`.
6. **Manual one-time test**: tag a `v0.5.0-rc1` against the worktree branch,
   let the release workflow run end-to-end on a fork, capture the smoke
   output to `sprints/sprint-9/evidence/US-9.1-smoke.txt`.
7. **/verify**: confirm the smoke job exits 0 and the tap repo `Formula/
   human.rb` has real (non-zero) SHA256s after the rc release.

## Risks

- **Backward compat (LOW / SMALL)**: No existing user installs human via
  Homebrew today (formula has never had real SHAs). Changing the formula's
  version and SHAs has zero impact on any existing install. Mitigation:
  none needed.
- **SHA256 mismatch / silent wrong-artifact download (LOW probability /
  LARGE impact)**: The script could match the wrong line and write a valid-
  looking but wrong hash, causing users to download a real-but-unintended
  binary. Mitigation: (a) `update-formula-hashes.sh` matches by URL context
  not just by placeholder pattern; (b) `check-formula-install.sh` does
  `brew fetch` BEFORE `brew install`, which forces a hash verification
  against the URL — if the hash is wrong the smoke job fails loudly; (c)
  the script exits non-zero if any of the three architectures is missed,
  so a partial update cannot ship.
- **Tampered binary served from GitHub releases (LOW / LARGE — security
  adjacent)**: An attacker with write access to `releases/` could replace a
  binary, and the formula's SHA would no longer match — but the formula
  would still be valid for the ORIGINAL hash, which is the safe behaviour
  (install fails, doesn't silently serve tampered code). The DANGEROUS case
  is if the attacker also rewrites the formula's SHA. **Detection of
  tampered-binary-with-matching-SHA is sprint-8's US-8.4 surface (codesign
  + notarization).** Coordinate via a comment in the formula:
  `# TODO(US-8.4): verify codesign signature once signing pipeline lands`.
  Do NOT add signature verification in this story — explicitly out of scope
  per sprint brief.
- **Tap PAT leak (LOW / MED)**: If `HUMANLABS_TAP_PUSH_TOKEN` is scoped only
  to `humanlabs/homebrew-human` and `contents:write`, the worst-case
  outcome is the tap is corrupted (recoverable: revert the tap commit,
  re-run release). Mitigation: PAT scoped to one repo, `contents:write`
  only, 90-day expiry, rotation in `.claude/operations` runbook.
- **`macos-latest` runner version drift (MED / SMALL)**: GitHub's
  `macos-latest` flipped from 14 → 15 → 26 over the past year; each flip
  can break `brew test` due to Xcode CLT version assumptions. Mitigation:
  pin `runs-on: macos-15` explicitly until macOS 26 image stabilises;
  re-evaluate every quarter.
- **`brew install --build-from-source` time on CI (MED / SMALL)**: cmake
  build of `human` with `-DHU_ENABLE_ALL_CHANNELS=ON` on a macos-15 runner
  takes ~6-9 min based on `ci.yml` history. The smoke job will be the
  long pole of the release. Mitigation: acceptable for release-only CI
  (releases happen <weekly, not on every PR). Do NOT add this job to
  `ci.yml`.
- **Observability (LOW)**: If a release ships and the tap update silently
  fails, users see a stale formula. Mitigation: `update-tap` job must
  `exit 1` on any git push failure; release workflow does not mark
  success until all of `release`, `update-tap`, and `brew-install-smoke`
  pass. Add a `gh release edit --draft` rollback step? — NO, out of scope;
  human operator inspects on failure.

## Test strategy

- **Script unit tests** (`tests/scripts/test_update_formula_hashes.sh`):
  4 cases — happy path, missing arch, malformed sums, idempotent re-run.
- **CI smoke** (`brew-install-smoke` job, release workflow only): exercises
  the real `brew tap` + `brew install` + `brew test` path on `macos-15`.
- **No C tests added** — no C code changes. The formula and scripts are the
  surface.
- **Manual evidence**: a `v0.5.0-rc1` release on a fork, captured in
  `sprints/sprint-9/evidence/US-9.1-smoke.txt`, demonstrates the full
  end-to-end path before tagging the real `v0.5.0`.

## Acceptance criteria mapping

- **AC-9.1.1** (`brew install` produces working binary, semver matches tag):
  covered by `check-formula-install.sh` steps (c) + (d).
- **AC-9.1.2** (pre-built path installs without cmake/Xcode CLT): NOT
  directly testable in CI because `macos-latest` has Xcode CLT pre-
  installed. Verified by inspection of the formula (`build.head?` branch is
  the only cmake path; pre-built path is `bin.install Dir["human-*"].first
  => "human"`). Documented in PR description with a manual smoke from a
  fresh macOS VM (implementer's responsibility once tap exists).
- **AC-9.1.3** (`--build-from-source` succeeds + tests pass + `brew test`
  passes): covered by `check-formula-install.sh` steps (b) + (e). Note:
  the formula's `def install` does not run `./build/human_tests` — it only
  builds the `human` target. Stories ACs mention tests must pass; if
  strict reading is required, implementer adds a `system "cmake", "--build",
  "build", "--target", "human_tests"` followed by `system "./build/
  human_tests"` to `def install`. **RECOMMENDATION**: do NOT do this in
  the formula — `brew install` is for end users, not for running the test
  suite. Re-read AC-9.1.3 with the scrum-master and clarify whether the
  "tests pass" clause means "the build is from a commit whose tests pass
  in CI" (already true via release-only-on-green-CI) or "brew runs the
  tests during install" (would add ~5min to every user install — bad).
  See "Open question" below.
- **AC-9.1.4** (`brew test` runs `human --version` and matches `"human"`):
  already in `Formula/human.rb` line 89. Re-verified by
  `check-formula-install.sh` step (e).
- **AC-9.1.5** (SHA256s match real artifacts, `brew fetch` succeeds):
  covered by `update-formula-hashes.sh` (writes the hashes) AND
  `check-formula-install.sh` (a `brew fetch` is implicit in `brew
  install`). Add an explicit `brew fetch humanlabs/human/human` step to
  `check-formula-install.sh` to surface fetch-only failures distinctly
  from build failures.

## Open question for scrum-master / product-owner

AC-9.1.3 says "the build succeeds, tests pass (`./build/human_tests` exits 0)".
The current formula does not run `human_tests` during `brew install`, and
running it would add ~5 minutes to every user install — a poor UX. Two
readings:

- **Strict**: the formula MUST run `human_tests` during `brew install`.
  Implementer adds this to `def install`.
- **Liberal**: "tests pass" means "the release tag was cut from a commit
  whose CI is green" (already enforced — releases trigger only on tags,
  and tags are typically cut after CI green). The formula stays as-is.

**Recommendation**: liberal reading. Brew formulas conventionally do not
run upstream test suites during user-facing install. `brew test` is the
agreed test seam (and it runs `human --version`, per AC-9.1.4). If the
scrum-master confirms liberal reading, proceed as designed. If strict,
implementer adds 5 lines to `def install` and flags the UX cost in the PR.

Not blocking — design is executable under either reading; resolution can
happen during implementation review.

## Out of scope (explicitly)

- Real Homebrew bottles (`brew bottle`) — deferred until US-8.4 (signing).
- Linux brew (`brew install` on Linuxbrew) — formula has Linux paths but
  Linuxbrew is low-priority; smoke job is macOS-only.
- Windows installer, pip, apt, snap — sprint brief out of scope.
- Notarization / codesign — sprint-8 US-8.4 owns this; coordinate via TODO
  comment in formula.
- Auto-update mechanism (`human self-update`) — separate concern.

## RESULT_tech-lead=DESIGN_READY
