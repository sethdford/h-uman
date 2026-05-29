#!/usr/bin/env bash
# check-sqlite-includer-ratchet.sh
#
# T1 guard (DDD bounded-context refactor, Phase 0). The number of src/ files
# that directly `#include <sqlite3.h>` must only ever DECREASE. New domain code
# must reach SQLite through a memory repository (see
# docs/plans/2026-05-29-ddd-bounded-contexts/phase-3-memory-query-interface.md),
# never by grabbing the raw sqlite3* handle via hu_sqlite_memory_get_db().
#
# The engine + repository layers are the ONLY places sqlite3 is legal, so
# src/memory/engines/ and src/memory/repos/ are exempt.
#
# Ratchet: as each Phase-3 aggregate migrates and a file drops its sqlite
# include, lower BASELINE to lock the gain. The script tells you when to.
set -euo pipefail

# Measured 2026-05-29 at the start of Phase 0.
BASELINE=110

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

# `|| true`: both greps return 1 on zero matches; under `set -o pipefail`
# that would abort the script the moment the count legitimately reaches 0.
count=$({ grep -rln '#include <sqlite3.h>' src/ 2>/dev/null \
  | grep -vE 'src/memory/(engines|repos)/' || true; } \
  | wc -l | tr -d ' ')

echo "sqlite3.h includers (excl engines/repos): $count (ceiling $BASELINE)"

if [ "$count" -gt "$BASELINE" ]; then
  echo "FAIL: a new file added '#include <sqlite3.h>'. Domain code must use a" >&2
  echo "      memory repository, not the raw handle. New/excess includers:" >&2
  grep -rln '#include <sqlite3.h>' src/ 2>/dev/null \
    | grep -vE 'src/memory/(engines|repos)/' >&2
  exit 1
fi

if [ "$count" -lt "$BASELINE" ]; then
  echo "NOTE: count dropped below baseline — lower BASELINE to $count in" >&2
  echo "      scripts/check-sqlite-includer-ratchet.sh to lock the gain." >&2
fi

exit 0
