#!/usr/bin/env bash
# check-formula-install.sh — CI smoke test for the Homebrew install path.
#
# Runs on macOS only (Homebrew installs are macOS-arm64 first-class). Performs
# the full happy-path a user would: tap → install --build-from-source → run
# binary → verify semver matches release tag → brew test.
#
# Inputs (env):
#   GITHUB_REF_NAME   The release tag (e.g. v0.5.0). Falls back to first arg.
#   HUMAN_TAP_REPO    Override tap URL (defaults to humanlabs/homebrew-human).
#
# Exit codes:
#   0   install + version match + brew test all succeeded
#   1   any step failed (loud, with the offending command echoed)
#   2   ran on a non-macOS platform (not supported)
#
# Notes:
#   - This script is invoked from .github/workflows/release.yml's
#     brew-install-smoke job (release-only, runs on macos-15).
#   - Uses --build-from-source for the smoke because the pre-built binary
#     bottle path requires US-8.4 codesigning to land first; see TODO marker
#     in Formula/human.rb.

set -euo pipefail

TAG="${GITHUB_REF_NAME:-${1:-}}"
TAP_REPO="${HUMAN_TAP_REPO:-humanlabs/homebrew-human}"

if [[ -z "$TAG" ]]; then
  echo "error: release tag not supplied (set GITHUB_REF_NAME or pass as arg)" >&2
  exit 1
fi

# Strip leading 'v' if present — formula version is bare semver.
EXPECTED_VERSION="${TAG#v}"

case "$(uname -s)" in
  Darwin) ;;
  *) echo "error: brew install smoke is macOS-only (got: $(uname -s))" >&2; exit 2 ;;
esac

if ! command -v brew >/dev/null 2>&1; then
  echo "error: brew not on PATH (Homebrew not installed?)" >&2
  exit 1
fi

run() {
  echo "+ $*"
  "$@"
}

# Step 1: tap.
echo "==> Tapping $TAP_REPO"
run brew tap "$TAP_REPO"

# Step 2: build-from-source install. Note: pre-built binary path is deferred
# to US-8.4 (codesigning). For now, the smoke uses the head/build-from-source
# branch to validate end-to-end. Switch to default install path once US-8.4
# lands and bottles are published.
echo "==> Installing humanlabs/human/human (build-from-source)"
run brew install --build-from-source humanlabs/human/human

# Step 3: binary on PATH and reports a semver.
echo "==> Verifying installed binary"
if ! command -v human >/dev/null 2>&1; then
  echo "error: 'human' not on PATH after brew install" >&2
  exit 1
fi
VERSION_OUTPUT="$(human --version 2>&1)"
echo "human --version: $VERSION_OUTPUT"

# Step 4: semver match against the release tag.
# Extract a semver substring from --version output (binary may print "human 0.5.0" etc.).
ACTUAL_VERSION="$(printf '%s\n' "$VERSION_OUTPUT" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+([-+][A-Za-z0-9.+-]+)?' | head -n1 || true)"
if [[ -z "$ACTUAL_VERSION" ]]; then
  echo "error: could not extract a semver from 'human --version' output" >&2
  echo "       output was: $VERSION_OUTPUT" >&2
  exit 1
fi

if [[ "$ACTUAL_VERSION" != "$EXPECTED_VERSION" ]]; then
  echo "error: version mismatch" >&2
  echo "       installed: $ACTUAL_VERSION" >&2
  echo "       expected:  $EXPECTED_VERSION (from tag $TAG)" >&2
  exit 1
fi

# Step 5: brew test (covers AC-9.1.4 — formula's test do block).
echo "==> Running brew test humanlabs/human/human"
run brew test humanlabs/human/human

echo "ok: brew install smoke passed (version $ACTUAL_VERSION matches tag $TAG)"
