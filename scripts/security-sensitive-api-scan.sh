#!/usr/bin/env bash
# Track E0/E1.1 — informational scan for sensitive libc wrappers in high-risk dirs.
#
# Exit 0 always (does not gate CI). Set VERIFY_SECURITY_SCAN=strict to exit 1
# when new uncommented hits appear — opt-in for local triage.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

DIRS=(src/security src/gateway src/tools src/runtime)
PATTERN='(^\s*[^#/]*\b(system|popen|getenv)\s*\()'

echo "=== security-sensitive-api-scan (informational) ==="
hits=0
for d in "${DIRS[@]}"; do
  if [[ -d "$d" ]]; then
    while IFS= read -r line; do
      echo "$line"
      hits=$((hits + 1))
    done < <(grep -RInE --include='*.c' "$PATTERN" "$d" 2>/dev/null || true)
  fi
done

if [[ "$hits" -eq 0 ]]; then
  echo "(no matches for system(|popen(|getenv( in ${DIRS[*]})"
fi

if [[ "${VERIFY_SECURITY_SCAN:-}" == "strict" ]] && [[ "$hits" -gt 0 ]]; then
  echo "strict mode: $hits match(es) — review required." >&2
  exit 1
fi

exit 0
