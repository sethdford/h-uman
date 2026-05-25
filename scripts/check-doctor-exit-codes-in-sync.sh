#!/usr/bin/env bash
# scripts/check-doctor-exit-codes-in-sync.sh
#
# Sprint 54 US-C3.9 (Phase 1) — Doctor exit-code parity guard.
#
# Asserts that the four HU_DOCTOR_EXIT_* constants in
# include/human/doctor.h match the exit-code table in
# docs/guides/doctor.md. Fails fast on drift.
#
# Pre-commit hook calls this whenever either file is staged.
# Runtime budget: < 100ms.
#
# Exit codes:
#   0 — docs and source agree
#   1 — drift detected (with diagnostic output)
#   2 — usage error (file not found, etc.)

set -euo pipefail

HEADER="include/human/doctor.h"
DOC="docs/guides/doctor.md"

# Resolve relative to the repo root by climbing if needed.
if [[ ! -f "$HEADER" ]]; then
  # Try from script's parent dir.
  cd "$(dirname "$0")/.."
fi

if [[ ! -f "$HEADER" ]]; then
  echo "error: $HEADER not found (run from repo root)" >&2
  exit 2
fi
if [[ ! -f "$DOC" ]]; then
  echo "error: $DOC not found (run from repo root)" >&2
  exit 2
fi

# Extract constants from the C header. Format:
#   #define HU_DOCTOR_EXIT_OK          0
extract_header_codes() {
  grep -E '^\s*#define\s+HU_DOCTOR_EXIT_[A-Z_]+\s+[0-9]+' "$HEADER" \
    | awk '{print $2, $3}' \
    | sort
}

# Extract constants from the markdown table. Format:
#   | 0 | `HU_DOCTOR_EXIT_OK` | ... |
# Output: "<SYMBOL> <NUMBER>" per line, sorted.
extract_doc_codes() {
  grep -E '^\| *[0-9]+ *\| *`HU_DOCTOR_EXIT_' "$DOC" \
    | sed -E 's/^\| *([0-9]+) *\| *`(HU_DOCTOR_EXIT_[A-Z_]+)`.*/\2 \1/' \
    | sort
}

HEADER_CODES="$(extract_header_codes)"
DOC_CODES="$(extract_doc_codes)"

if [[ -z "$HEADER_CODES" ]]; then
  echo "error: no HU_DOCTOR_EXIT_* constants found in $HEADER" >&2
  exit 2
fi
if [[ -z "$DOC_CODES" ]]; then
  echo "error: no HU_DOCTOR_EXIT_* table rows found in $DOC" >&2
  exit 2
fi

if [[ "$HEADER_CODES" != "$DOC_CODES" ]]; then
  echo "DRIFT: $HEADER and $DOC disagree on exit codes." >&2
  echo "" >&2
  echo "From $HEADER:" >&2
  echo "$HEADER_CODES" | sed 's/^/  /' >&2
  echo "" >&2
  echo "From $DOC:" >&2
  echo "$DOC_CODES" | sed 's/^/  /' >&2
  echo "" >&2
  echo "Fix by editing whichever side is wrong. The constants in" >&2
  echo "$HEADER are the source of truth for the C compile; the table in" >&2
  echo "$DOC is the source of truth for the user-visible contract." >&2
  echo "They must match exactly." >&2
  exit 1
fi

# Success — silent unless --verbose.
if [[ "${1:-}" == "--verbose" ]]; then
  echo "OK: $HEADER and $DOC agree on $(echo "$HEADER_CODES" | wc -l | tr -d ' ') exit codes."
fi
exit 0
