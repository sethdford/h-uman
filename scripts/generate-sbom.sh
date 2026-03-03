#!/bin/sh
# Generate CycloneDX SBOM for seaclaw
# Usage: ./scripts/generate-sbom.sh [--help] [<source_dir>] [<output_file>]

set -eu

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

die() { printf "${RED}error:${NC} %s\n" "$1" >&2; exit 1; }
info() { printf "${GREEN}==>${NC} ${BOLD}%s${NC}\n" "$1"; }
warn() { printf "${YELLOW}warning:${NC} %s\n" "$1"; }

case "${1:-}" in
    --help|-h)
        printf "Usage: %s [<source_dir>] [<output_file>]\n" "$0"
        printf "Generate CycloneDX SBOM for seaclaw. Defaults: source_dir=., output_file=sbom.json\n"
        exit 0
        ;;
esac

SRC_DIR="${1:-.}"
OUT="${2:-sbom.json}"
VERSION=$(git -C "$SRC_DIR" describe --tags --always 2>/dev/null || echo "0.0.0-dev")
TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

# Generate UUID: uuidgen (macOS), /proc (Linux), or fallback
UUID="00000000-0000-0000-0000-000000000000"
if command -v uuidgen >/dev/null 2>&1; then
    UUID=$(uuidgen 2>/dev/null || true)
elif [ -r /proc/sys/kernel/random/uuid ] 2>/dev/null; then
    UUID=$(cat /proc/sys/kernel/random/uuid 2>/dev/null || true)
fi

# Ensure output directory exists
OUT_DIR=$(dirname "$OUT")
mkdir -p "$OUT_DIR"

cat > "$OUT" << SBOM_EOF
{
  "bomFormat": "CycloneDX",
  "specVersion": "1.5",
  "serialNumber": "urn:uuid:$UUID",
  "version": 1,
  "metadata": {
    "timestamp": "$TIMESTAMP",
    "tools": [{ "name": "seaclaw-sbom-generator", "version": "1.0.0" }],
    "component": {
      "type": "application",
      "name": "seaclaw",
      "version": "$VERSION",
      "description": "Autonomous AI assistant runtime",
      "licenses": [{ "license": { "id": "MIT" } }]
    }
  },
  "components": [
    {
      "type": "library",
      "name": "libc",
      "description": "C standard library (system)",
      "scope": "required"
    },
    {
      "type": "library",
      "name": "sqlite3",
      "description": "SQLite database engine",
      "scope": "optional",
      "properties": [{ "name": "build-flag", "value": "SC_ENABLE_SQLITE" }]
    },
    {
      "type": "library",
      "name": "libcurl",
      "description": "HTTP client library",
      "scope": "optional",
      "properties": [{ "name": "build-flag", "value": "SC_ENABLE_CURL" }]
    },
    {
      "type": "library",
      "name": "openssl",
      "description": "Cryptographic library (FIPS mode)",
      "scope": "optional",
      "properties": [{ "name": "build-flag", "value": "SC_ENABLE_FIPS_CRYPTO" }]
    }
  ]
}
SBOM_EOF

echo "SBOM written to $OUT"
