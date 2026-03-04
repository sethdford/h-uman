#!/usr/bin/env bash
set -euo pipefail

# Usage: scripts/prepare-release.sh v0.3.0
# Updates version references across the project and creates a release commit.

VERSION="${1:?Usage: $0 <version> (e.g. v0.3.0)}"
SEMVER="${VERSION#v}"

echo "Preparing release ${VERSION}..."

# Update CHANGELOG.md date
TODAY=$(date +%Y-%m-%d)
sed -i.bak "s/\[${SEMVER}\] - .*/[${SEMVER}] - ${TODAY}/" CHANGELOG.md && rm -f CHANGELOG.md.bak

# Update README.md test count (optional — skip if not found)
# This is informational only; CI verifies the real count.

echo "Release ${VERSION} prepared."
echo "Next steps:"
echo "  1. Review changes: git diff"
echo "  2. Commit: git commit -am 'release: ${VERSION}'"
echo "  3. Tag: git tag ${VERSION}"
echo "  4. Push: git push origin main ${VERSION}"
