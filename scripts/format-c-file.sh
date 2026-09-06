#!/usr/bin/env bash
# format-c-file.sh — clang-format ONE C/H file, but only when its braces balance.
#
# Why: the on-save PostToolUse hook used to run clang-format unconditionally.
# On 2026-09-02 an Edit that briefly left src/daemon.c with one extra `}` made
# clang-format re-indent ~3,000 lines of the enclosing function — the diff
# became unreviewable and the file had to be restored from HEAD. A formatter
# must never touch a file that is not syntactically closed.
#
# Usage: format-c-file.sh <path.c|path.h>
# Exit: always 0 (a hook must not block the edit); prints one line on skip.
set -uo pipefail

file="${1:-}"
[[ -n "$file" && -f "$file" ]] || exit 0
case "$file" in *.c|*.h) ;; *) exit 0 ;; esac

# Brace balance with comments, string and char literals stripped. Heuristic,
# not a parser: preprocessor-conditional brace tricks can fool it, in which
# case we err on the side of NOT formatting (the edit still stands).
balance="$(python3 - "$file" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)          # block comments
src = re.sub(r"//[^\n]*", "", src)                        # line comments
src = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', src)           # string literals
src = re.sub(r"'(?:\\.|[^'\\\n])*'", "''", src)           # char literals
print(src.count("{") - src.count("}"))
PY
)" || balance="?"

if [[ "$balance" != "0" ]]; then
    echo "format-c-file: SKIP $file — braces unbalanced (delta=$balance); fix the edit, then format" >&2
    exit 0
fi

fmt="$(command -v clang-format 2>/dev/null || true)"
[[ -x "$fmt" ]] || fmt=/opt/homebrew/opt/llvm/bin/clang-format
[[ -x "$fmt" ]] || exit 0
"$fmt" -i "$file" 2>/dev/null || true
exit 0
