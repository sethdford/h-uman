# Critic findings — US-9.1 Homebrew install path

## HIGH (2)

- `.github/workflows/release.yml`:no concurrency key — two release tags pushed
  in rapid succession (e.g. `v0.5.0` then `v0.5.1` before the first
  `update-tap` job finishes) both clone `humanlabs/homebrew-human`, both commit
  to the default branch, and the second `git push` races the first. One push
  will fail with a non-fast-forward error, leaving the tap at the earlier
  version with no retry. Fix: add `concurrency: group: tap-update
  cancel-in-progress: false` to the `update-tap` job so the second run queues
  rather than races.

- `.github/workflows/release.yml`:229-237 — `git clone` of
  `humanlabs/homebrew-human` has no failure-mode distinction between "repo does
  not exist" (404, first-ever release bootstrap) and "PAT is wrong" (403). Both
  fail the clone with the same opaque git error; the operator cannot tell whether
  to create the repo or rotate the secret. Fix: add a `gh repo view
  humanlabs/homebrew-human` preflight (using `GITHUB_TOKEN`) before the clone;
  emit a clear `::error::` annotation on 404 naming the bootstrap step.

## MED (3)

- `.github/workflows/release.yml`:34 — cross-compiler install step has
  `continue-on-error: true`. If `apt-get install gcc-aarch64-linux-gnu` fails,
  the build continues and silently produces a native x86_64 binary named
  `human-linux-aarch64.bin`. The SHA then lands in the formula under the wrong
  architecture URL. Fix: remove `continue-on-error: true` and let the step fail
  loudly; or add a post-step check that the cross-compiler binary is on PATH
  before proceeding.

- `tests/test_update_formula_hashes.sh` — no test exercises the case where
  `sha256sums.txt` is present and readable but **empty** (zero bytes). The
  script's `HAVE_ANY=0` guard catches this, but the test for "malformed sums"
  (T4) uses comment-only content, not a zero-byte file. An empty file is a
  distinct OS-level failure mode (truncated upload). Fix: add T4b with
  `touch "$T4b/sums.txt"` and assert exit 3.

- `Formula/human.rb`:61 — `bin.install Dir["human-*"].first => "human"` silently
  installs nothing if the glob matches zero files (e.g. the release artifact was
  renamed or the download failed). `Dir[].first` returns `nil`; Ruby's
  `bin.install nil => "human"` raises at runtime inside a user's `brew install`,
  giving a confusing backtrace rather than a clear error. Fix: assign to a local,
  check for nil, and `odie "no pre-built binary found"` explicitly.

## LOW (2)

- `.github/workflows/release.yml`:272 — `brew-install-smoke` exports
  `GITHUB_REF_NAME` as an env var into the step but `check-formula-install.sh`
  also accepts a positional `$1`. The duplication is harmless but creates two
  code paths to maintain. Fix: drop the env override; the script's default
  `${GITHUB_REF_NAME:-${1:-}}` picks it up from the runner env without explicit
  re-export.

- `Formula/human.rb`:28-33 — the `TODO(US-8.4)` comment for the `bottle do`
  block has no machine-readable guard preventing a future automated tool from
  inserting `bottle do` silently. Fix: add a CI assertion (e.g. in
  `scripts/check-formula-install.sh` or a pre-commit check) that `human.rb`
  does NOT contain `bottle do`, failing loudly if it appears before US-8.4
  lands.

## Cross-agent regression risk

- None detected. No C source, headers, or CMake targets were modified by this
  story. The only shared file touched outside the formula/scripts/workflow
  surface is `CMakeLists.txt` (listed in `git diff --name-only` output) — if
  that change is load-bearing for other sprint-9 agents building C code,
  review it independently.

