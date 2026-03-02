#!/bin/sh
# Generate CycloneDX SBOM for seaclaw
# Usage: generate-sbom.sh <source_dir> <output_file>

set -e

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
