#!/usr/bin/env bash
# check-no-date-time-macros.sh
#
# US-8.3 — Reproducible Binary Builds. Belt-and-suspenders alongside
# -Werror=date-time in CMakeLists.txt. Greps for any use of the
# __DATE__ or __TIME__ preprocessor macros in tracked C source under
# src/ and include/. These macros embed the build timestamp into the
# binary and are the most common source of irreproducibility.
#
# Exit codes:
#   0  no offending uses found
#   1  at least one offending use found (or required tool missing)
#   2  invocation error (bad arguments)
#
# Rationale: -Werror=date-time fails the compile when used in
# preprocessed output, but a grep is cheaper to run in CI gates that
# do not compile (e.g. doc-gate jobs) and gives a clean grep-style
# diagnostic. Both gates must pass.
#
# Anti-pattern AP-1 from sprints/sprint-8/designs/US-8.3.md: silent
# diff acceptance. This script MUST exit nonzero on any violation;
# it MUST NOT print a warning and continue.

set -euo pipefail

# Resolve repo root from the script location so this works from any cwd.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

usage() {
    cat <<'EOF'
Usage: check-no-date-time-macros.sh [--help]

Greps src/ and include/ in the repo root for __DATE__ or __TIME__
preprocessor macro use. Exits 0 if none found, 1 if any found.

This script is wired into CI (.github/workflows/ci.yml job
reproducible-build) and the design recommends running it locally
before any CMakeLists.txt change.
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi

if [[ $# -gt 0 ]]; then
    echo "error: unexpected argument: $1" >&2
    usage >&2
    exit 2
fi

cd "$REPO_ROOT"

# Use ripgrep when available (faster, respects .gitignore); fall back to
# grep -r so the script works on stock systems without rg.
if command -v rg >/dev/null 2>&1; then
    # -n line numbers, -F fixed string (faster than regex), --no-heading
    # for parseable output. Search src/ and include/ only.
    OUT_DATE=$(rg -n -F '__DATE__' --no-heading src/ include/ || true)
    OUT_TIME=$(rg -n -F '__TIME__' --no-heading src/ include/ || true)
else
    OUT_DATE=$(grep -RIn -F '__DATE__' src/ include/ || true)
    OUT_TIME=$(grep -RIn -F '__TIME__' src/ include/ || true)
fi

FOUND=0
if [[ -n "$OUT_DATE" ]]; then
    echo "error: __DATE__ macro use found (breaks reproducibility):" >&2
    echo "$OUT_DATE" >&2
    FOUND=1
fi
if [[ -n "$OUT_TIME" ]]; then
    echo "error: __TIME__ macro use found (breaks reproducibility):" >&2
    echo "$OUT_TIME" >&2
    FOUND=1
fi

if [[ $FOUND -ne 0 ]]; then
    cat >&2 <<'EOF'

US-8.3 reproducibility contract violated.

__DATE__ and __TIME__ expand to the wall-clock time at compile,
embedding nondeterminism into the binary. Two builds from the same
source will produce different SHA-256 hashes.

Fix: remove the macro. If you need a build version, use the
HU_VERSION macro from include/human/version.h, or read
SOURCE_DATE_EPOCH from the environment at build time.

See: sprints/sprint-8/designs/US-8.3.md
     docs/standards/engineering/reproducible-builds.md (if present)
EOF
    exit 1
fi

echo "check-no-date-time-macros: OK (no __DATE__ or __TIME__ in src/ or include/)"
exit 0
