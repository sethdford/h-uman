#!/bin/sh
# One-click release script for SeaClaw.
# Usage: ./scripts/release.sh [version]
# Example: ./scripts/release.sh 2026.3.15
#
# What it does:
#   1. Validates the version format
#   2. Runs the full test suite
#   3. Updates version in CMakeLists.txt and src/main.c
#   4. Generates changelog entry from commits since last tag
#   5. Commits the version bump
#   6. Creates and pushes the git tag
#
# The tag push triggers .github/workflows/release.yml which:
#   - Builds Linux x86_64 + macOS aarch64 binaries
#   - Creates a GitHub Release with binaries + extras
#   - Builds and pushes Docker image to ghcr.io

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

die() { printf "${RED}error:${NC} %s\n" "$1" >&2; exit 1; }
info() { printf "${GREEN}==>${NC} ${BOLD}%s${NC}\n" "$1"; }
warn() { printf "${YELLOW}warning:${NC} %s\n" "$1"; }

case "${1:-}" in
    -h|--help)
        echo "Usage: $(basename "$0") [version]"
        echo "  version  Semantic version (e.g. 0.2.0, 2026.3.15)"
        echo ""
        echo "If no version is given, prompts interactively."
        echo "Runs tests, bumps version, generates changelog, creates git tag."
        exit 0
        ;;
esac

VERSION="${1:-}"
if [ -z "$VERSION" ]; then
    LAST_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "v0.0.0")
    printf "Last release: %s\n" "$LAST_TAG"
    printf "Enter new version (e.g. 2026.3.15 or 0.2.0): "
    read -r VERSION
fi

[ -z "$VERSION" ] && die "Version required"

TAG="v${VERSION}"

if git rev-parse "$TAG" >/dev/null 2>&1; then
    die "Tag $TAG already exists"
fi

if [ -n "$(git status --porcelain)" ]; then
    die "Working tree is dirty. Commit or stash changes first."
fi

info "Running tests..."
JOBS=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
BUILD_DIR="build-check"
mkdir -p "$BUILD_DIR"
(cd "$BUILD_DIR" && cmake .. -DCMAKE_BUILD_TYPE=Debug 2>/dev/null && cmake --build . -j"$JOBS" 2>&1 | tail -1)
(cd "$BUILD_DIR" && ./seaclaw_tests) || die "Tests failed. Fix before releasing."

info "Running benchmark and saving to history..."
RELEASE_BUILD_DIR="build-release"
mkdir -p "$RELEASE_BUILD_DIR"
(cd "$RELEASE_BUILD_DIR" && cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON -DSC_ENABLE_ALL_CHANNELS=ON 2>/dev/null && cmake --build . -j"$JOBS" 2>&1 | tail -1)
if [ -x "$RELEASE_BUILD_DIR/seaclaw" ]; then
    "$ROOT/scripts/benchmark.sh" "$RELEASE_BUILD_DIR/seaclaw" --save-history 2>&1 | grep -E "Appended|Created|Results saved"
else
    warn "Release binary not found — skipping benchmark"
fi

info "Updating version to $VERSION..."

if grep -q 'project(seaclaw VERSION' CMakeLists.txt; then
    sed -i.bak "s/project(seaclaw VERSION [^ )]*)/project(seaclaw VERSION $VERSION)/" CMakeLists.txt && rm -f CMakeLists.txt.bak
fi

sed -i.bak "s/SeaClaw v[0-9][0-9.]*/SeaClaw v$VERSION/g" CMakeLists.txt && rm -f CMakeLists.txt.bak

if grep -q '#define SC_VERSION' src/main.c; then
    sed -i.bak "s/#define SC_VERSION \"[^\"]*\"/#define SC_VERSION \"$VERSION\"/" src/main.c && rm -f src/main.c.bak
fi

if grep -q '#define SC_VERSION' src/version.c; then
    sed -i.bak "s/#define SC_VERSION \"[^\"]*\"/#define SC_VERSION \"$VERSION\"/" src/version.c && rm -f src/version.c.bak
fi

if [ -f flake.nix ] && grep -q 'version = ' flake.nix; then
    sed -i.bak "s/version = \"[^\"]*\"/version = \"$VERSION\"/" flake.nix && rm -f flake.nix.bak
fi

info "Generating changelog entry..."
if command -v git-cliff >/dev/null 2>&1; then
    git-cliff --unreleased --tag "$TAG" > /tmp/cliff_entry.md 2>/dev/null
    if [ -s /tmp/cliff_entry.md ] && [ -f CHANGELOG.md ]; then
        HEADER=$(head -5 CHANGELOG.md)
        BODY=$(tail -n +6 CHANGELOG.md)
        printf '%s\n\n%s\n%s\n' "$HEADER" "$(cat /tmp/cliff_entry.md)" "$BODY" > CHANGELOG.md
    fi
else
    warn "git-cliff not found, falling back to git log"
    LAST_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "")
    if [ -n "$LAST_TAG" ]; then
        COMMITS=$(git log "${LAST_TAG}..HEAD" --oneline --no-decorate)
    else
        COMMITS=$(git log --oneline --no-decorate -20)
    fi
    DATE=$(date +%Y-%m-%d)
    ENTRY="## [$VERSION] - $DATE

### Changed
$(echo "$COMMITS" | sed 's/^[0-9a-f]* /- /')
"
    if [ -f CHANGELOG.md ]; then
        HEADER=$(head -5 CHANGELOG.md)
        BODY=$(tail -n +6 CHANGELOG.md)
        printf '%s\n\n%s\n%s\n' "$HEADER" "$ENTRY" "$BODY" > CHANGELOG.md
    fi
fi

info "Committing version bump..."
git add -A
git commit -m "release: $TAG"

info "Creating tag $TAG..."
git tag -a "$TAG" -m "Release $TAG"

printf "\n"
info "Ready to push!"
printf "  git push origin main && git push origin %s\n\n" "$TAG"
printf "Push now? [y/N] "
read -r CONFIRM
case "$CONFIRM" in
    [yY]|[yY][eE][sS])
        git push origin main
        git push origin "$TAG"
        info "Pushed! Release workflow will build binaries and create the GitHub Release."
        ;;
    *)
        warn "Skipped push. Run manually when ready:"
        printf "  git push origin main && git push origin %s\n" "$TAG"
        ;;
esac
