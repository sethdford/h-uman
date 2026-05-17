#!/usr/bin/env bash
# Sprint 2c F5 — flag extern symbols that won't link in HU_IS_TEST builds.
#
# Sprint 1 Story C was blocked for ~30 minutes by a subtle failure mode:
# `hu_starter_persona_json` was declared `extern const char [...]` in
# `include/human/onboard.h`, but its definition lived inside the
# `#else` branch of an `#ifdef HU_IS_TEST` block. So the symbol was
# only compiled when `HU_IS_TEST` was undefined — invisible to the
# test build that needed to link against it.
#
# This lint catches that pattern statically:
#
#   1. Collect every `extern <type> NAME[...];` declaration from
#      `include/**/*.h`.
#   2. Find each symbol's definition in `src/**/*.c`.
#   3. For each definition site, walk the active preprocessor guard
#      stack and flag any case where the symbol is omitted when
#      HU_IS_TEST is defined:
#        - inside `#ifdef HU_IS_TEST` 'else' branch
#        - inside `#ifndef HU_IS_TEST` 'if' branch
#        - inside `#if !defined(HU_IS_TEST)` 'if' branch
#
# The lint does NOT flag:
#   - test-only symbols declared in `tests/`-local headers (only public
#     headers under `include/` are the source of declarations)
#   - production-only symbols never declared `extern` in a public
#     header (e.g., static helpers, main())
#   - guards keyed on something OTHER than HU_IS_TEST (those are
#     legitimate platform-specific variations)
#
# Override the scan root via HU_TESTSYM_SCAN_ROOT (test-only). Same
# pattern as `check-memory-query-variant.sh` (Sprint 2b Story D).
set -euo pipefail

if [[ -n "${HU_TESTSYM_SCAN_ROOT:-}" ]]; then
  cd "$HU_TESTSYM_SCAN_ROOT"
else
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  cd "$ROOT"
fi

if ! command -v python3 &>/dev/null; then
  echo "check-test-time-symbol-availability: python3 not found; skip." >&2
  exit 0
fi

python3 <<'PY'
import re
import sys
from pathlib import Path

ROOT = Path(".")

# 1. Collect extern declarations from public headers.
#    Match: extern [const] [struct X | typedef-name] NAME[...]; (semicolon required, no init)
#    Skip function decls (parens after name).
extern_re = re.compile(
    r"""
    ^\s*extern\s+
    (?:const\s+)?                          # optional const
    (?:struct\s+\w+|\w+(?:\s+\*+)?)\s+     # type name (struct X | foo | foo*)
    (\w+)\s*                               # symbol
    (?:\[\s*[\w]*\s*\])?\s*                # optional array []
    ;\s*(?:/\*.*?\*/|//.*)?\s*$            # semicolon, optional trailing comment
    """,
    re.VERBOSE | re.MULTILINE | re.DOTALL,
)
# Reject obvious function decls: an open-paren before semicolon.
fn_decl_re = re.compile(r"\([^;]*\)\s*;")

declared = {}  # symbol -> header path
for header in ROOT.glob("include/**/*.h"):
    text = header.read_text(encoding="utf-8", errors="replace")
    # Strip comments first so commented-out externs don't trigger.
    cleaned = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    cleaned = re.sub(r"//[^\n]*", "", cleaned)
    for m in extern_re.finditer(cleaned):
        sym = m.group(1)
        # Bail if the same line contains a function-decl shape.
        line_text = cleaned[m.start():m.end()]
        if fn_decl_re.search(line_text):
            continue
        declared.setdefault(sym, str(header))

# 2. For each declared symbol, find definitions in src/ and check guards.
def_re_template = (
    r"^[^\S\n]*"
    r"(?:const\s+)?"
    r"(?:struct\s+\w+|\w+(?:\s+\*+)?)\s+"
    r"{sym}\s*"
    r"(?:\[\s*\w*\s*\])?\s*"
    r"=\s*"
)

def is_test_guard(condition):
    return bool(re.search(r"\bHU_IS_TEST\b|\b_HU_TEST\b", condition))

def walk_guard_stack(text, def_line):
    """Return the active preprocessor guard chain at `def_line` (1-based).

    Each entry: (directive, condition, branch) where:
      directive ∈ {"ifdef","ifndef","if"}
      condition is the raw expression text
      branch ∈ {"if","else"}  (whether we're in the THEN or ELSE side)
    """
    stack = []
    lines = text.splitlines()
    for j in range(min(def_line - 1, len(lines))):
        s = lines[j].strip()
        m = re.match(r"#\s*(ifdef|ifndef|if)\s+(.+)", s)
        if m:
            stack.append([m.group(1), m.group(2).strip(), "if"])
            continue
        if re.match(r"#\s*elif\b", s):
            continue  # treat as same nesting level
        if re.match(r"#\s*else\b", s):
            if stack:
                stack[-1][2] = "else"
            continue
        if re.match(r"#\s*endif\b", s):
            if stack:
                stack.pop()
            continue
    return stack

bad = []
for sym, header in sorted(declared.items()):
    def_re = re.compile(def_re_template.format(sym=re.escape(sym)), re.MULTILINE)
    for src in ROOT.glob("src/**/*.c"):
        text = src.read_text(encoding="utf-8", errors="replace")
        if sym not in text:
            continue
        for m in def_re.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            stack = walk_guard_stack(text, line)
            for directive, cond, branch in stack:
                if not is_test_guard(cond):
                    continue
                # Decide whether the symbol is OMITTED when HU_IS_TEST is defined.
                excluded = False
                why = ""
                if directive == "ifdef" and branch == "else":
                    # `#ifdef HU_IS_TEST ... #else <DEF> #endif`
                    excluded = True
                    why = f"#ifdef {cond} 'else' branch — symbol absent when HU_IS_TEST is defined"
                elif directive == "ifndef" and branch == "if":
                    # `#ifndef HU_IS_TEST <DEF> ... #endif`
                    excluded = True
                    why = f"#ifndef {cond} 'if' branch — symbol absent when HU_IS_TEST is defined"
                elif directive == "if" and branch == "if" and re.search(r"!\s*defined\s*\(\s*HU_IS_TEST\s*\)", cond):
                    excluded = True
                    why = f"#if !defined(HU_IS_TEST) 'if' branch — symbol absent when HU_IS_TEST is defined"
                if excluded:
                    bad.append((str(src), line, sym, header, why))
                    break  # one diagnostic per definition site

for src, line, sym, header, why in bad:
    print(
        f"{src}:{line}: extern symbol `{sym}` (declared in {header}): {why}",
        file=sys.stderr,
    )

if bad:
    print(
        f"\n{len(bad)} extern symbol(s) won't link in HU_IS_TEST builds. "
        "Move the definition above the HU_IS_TEST guard so both production "
        "and test builds compile it.",
        file=sys.stderr,
    )
    sys.exit(1)
PY
