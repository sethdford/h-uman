#!/usr/bin/env bash
# Track B2 — require explicit hu_memory_query_t.variant after stack zero-init.
#
# Flags memset(&VAR, 0, sizeof(hu_memory_query_t)) and memset(&VAR, 0, sizeof(VAR))
# when the preceding lines declare `hu_memory_query_t VAR`. The next 48 lines
# must contain `.variant =`.
#
# Override the scan root via HU_VARIANT_SCAN_ROOT (test-only). When unset,
# the scanner runs against the repo root (script-relative). Sprint 2b
# Story D negative test exercises this override to inject bad fixtures
# without polluting the live tree.
set -euo pipefail

if [[ -n "${HU_VARIANT_SCAN_ROOT:-}" ]]; then
  cd "$HU_VARIANT_SCAN_ROOT"
else
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  cd "$ROOT"
fi

if ! command -v python3 &>/dev/null; then
  echo "check-memory-query-variant: python3 not found; skip." >&2
  exit 0
fi

python3 <<'PY'
import re
import sys
from pathlib import Path

root = Path(".")
paths = list(root.glob("src/**/*.c")) + list(root.glob("tests/**/*.c"))

decl_re = re.compile(r"^\s*hu_memory_query_t\s+(\w+)\s*;\s*$")
# memset(&id, 0, sizeof(hu_memory_query_t)) OR sizeof(id)
mem_re = re.compile(
    r"memset\s*\(\s*&(\w+)\s*,\s*0\s*,\s*sizeof\s*\(\s*(?:hu_memory_query_t|\1)\s*\)\s*\)\s*;"
)

# Sprint 2c F3 — strip C and C++ comments before scanning. Without this
# the lookahead window for `.variant =` matched comments like
# `/* No .variant = X */`, falsely accepting code that never assigned
# the variant. Replace comments with same-length whitespace so line
# numbers stay correct in diagnostics. C++ `// ...` comments stop at
# end-of-line (no greedy multi-line risk). C `/* ... */` are matched
# non-greedy across newlines.
def strip_comments(text):
    def blank_run(match):
        return re.sub(r"[^\n]", " ", match.group(0))
    text = re.sub(r"/\*.*?\*/", blank_run, text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", blank_run, text)
    return text

bad = []
for path in sorted(paths):
    raw = path.read_text(encoding="utf-8", errors="replace")
    text = strip_comments(raw)
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = mem_re.search(line)
        if not m:
            continue
        var = m.group(1)
        # Walk backward a few lines for declaration of this hu_memory_query_t
        decl_ok = False
        for j in range(max(0, i - 6), i):
            dm = decl_re.match(lines[j])
            if dm and dm.group(1) == var:
                decl_ok = True
                break
        if not decl_ok:
            continue
        window = "\n".join(lines[i : min(len(lines), i + 48)])
        if ".variant" not in window or not re.search(r"\.\s*variant\s*=", window):
            bad.append(f"{path}:{i+1}: memset(&{var},...) without `.variant =` in following 48 lines")

for msg in bad:
    print(msg, file=sys.stderr)
if bad:
    sys.exit(1)
PY
